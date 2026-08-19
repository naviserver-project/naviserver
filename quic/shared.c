/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
 * Copyright (C) 2025 Gustaf Neumann
 */

/*
 *======================================================================
 * shared.c: Shared stream state, chunk queues, and resume ring for the
 *           HTTP/3 driver
 *======================================================================
 *
 * Overview
 * --------
 * This module provides small, thread-safe primitives used by the HTTP/3
 * (QUIC) driver:
 *
 *   - Chunk / ChunkQueue:
 *       A zero-copy, singly-linked FIFO of payload "chunks". Each chunk
 *       stores its payload inline (one allocation), and moves between
 *       queues by relinking (no memcpy). Typical use is per-stream TX:
 *       "queued" (app-owned) -> "pending" (about to write on wire).
 *
 *   - SharedStream:
 *       Per-stream state with a mutex protecting TX queues, header/close
 *       flags, and lightweight counters. Helpers build nghttp3_vec views
 *       over the pending queue and trim it after successful writes.
 *       (See: https://nghttp2.org/nghttp3/)
 *
 *   - SharedState (resume ring):
 *       A lock-protected circular FIFO of int64_t stream IDs (SIDs) used
 *       to coalesce wakeups. Producers call SharedRequestResume() to push
 *       a SID once (guarded by ss->resume_enqueued) and optionally issue
 *       an edge-triggered wake via a driver-provided callback. Consumers
 *       pop/drain SIDs and clear the per-stream flag after handling.
 *
 * Concurrency model
 * -----------------
 * - tx_queued and tx_state are shared between the producer and the
 *   QUIC thread and are protected by ss->lock.
 * - tx_pending is owned exclusively by the QUIC thread after transfer
 *   from tx_queued.
 * - The resume_enqueued flag is both tested/set and cleared under
 *   st->lock. Using the same mutex for all accesses keeps the flag
 *   synchronized with membership in the resume ring and prevents lost
 *   or duplicate resume requests.
 *
 * Memory & logging
 * ----------------
 * - Chunks are allocated via ns_malloc(sizeof(Chunk) + payload) and freed
 *   by trim/clear helpers; moving between queues never copies data.
 * - Functions here are generally allocation-free except for Chunk* and
 *   resume ring growth (resume_grow()). Logging is conservative and at
 *   Notice level in debug helpers.
 *
 * Typical flow (TX)
 * -----------------
 *   SharedEnqueueBody()         -> enqueue app data (no resume)
 *   SharedRequestResume()       -> push SID to ring and edge-wake worker
 *   worker: SharedDrainResume() -> pop SIDs to service
 *   worker: SharedBuildVecsFromPending() -> get nghttp3_vecs
 *   write via nghttp3/OpenSSL   -> on success, SharedTrimPending*()
 *   worker: SharedResumeClear() -> allow future re-enqueue
 *
 * Notes
 * -----
 * - Mutex destruction and object lifetime are managed by the embedding
 *   driver; Destroy() helpers here free queues but do not destroy mutexes.
 * - The resume ring stores SIDs only; streams are looked up by the driver.
 */


#include "../nsd/nsd.h"

#if defined(HAVE_NGHTTP3)
#include "shared.h"

/* ---------- Prototypes ---------- */
static int  resume_grow(SharedState *st) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_LogSeverity Ns_LogQuicDebug;

#ifdef NS_DRIVER_MEM_STATS
# define SHARED_COUNTER_STATS 1
#endif

#ifdef SHARED_COUNTER_STATS

NS_EXTERN void NsSharedCounterStatsLog(uint64_t iter);

typedef struct SharedCounters {
    uint64_t stream_destroy_lock;
    uint64_t stream_enqueue_lock;
    uint64_t stream_splice_lock;
    uint64_t resume_request_lock;
    uint64_t resume_drain_lock;
} SharedCounters;

typedef struct SharedCounterStats {
    Ns_Mutex       lock;
    SharedCounters counters;
} SharedCounterStats;

static SharedCounterStats sharedCounterStats;

static void
SharedCounterStatsIncr(uint64_t *counterPtr)
{
    Ns_MutexLock(&sharedCounterStats.lock);
    (*counterPtr)++;
    Ns_MutexUnlock(&sharedCounterStats.lock);
}

void
NsSharedCounterStatsLog(uint64_t iter)
{
    SharedCounters snapshot;

    Ns_MutexLock(&sharedCounterStats.lock);
    snapshot = sharedCounterStats.counters;
    Ns_MutexUnlock(&sharedCounterStats.lock);

Ns_Log(Notice,
       "[%lld] H3 shared locks:"
       " stream_destroy_lock %llu"
       " stream_enqueue_lock %llu"
       " stream_splice_lock %llu"
       " resume_request_lock %llu"
       " resume_drain_lock %llu"
       , iter,
       (unsigned long long)snapshot.stream_destroy_lock,
       (unsigned long long)snapshot.stream_enqueue_lock,
       (unsigned long long)snapshot.stream_splice_lock,
       (unsigned long long)snapshot.resume_request_lock,
       (unsigned long long)snapshot.resume_drain_lock
       );
}
#else
# define SharedCounterStatsIncr(c)
#endif /* SHARED_COUNTER_STATS */

/*======================================================================
 * Function Implementations: Utilities
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * resume_grow --
 *
 *      Increase the capacity of a full resume ring, preserving all
 *      queued stream IDs in FIFO order.
 *
 *      The ring is expected to be full on entry (count == cap). For an
 *      unallocated ring, allocate the initial capacity. If a full
 *      ring is wrapped, relocate its two occupied portions into a
 *      contiguous region of the enlarged allocation. If it is already
 *      linear, advance tail to the first newly available slot.
 *
 *      The caller must hold st->lock.
 *
 * Results:
 *      0 on success, -1 on memory allocation failure.
 *
 * Side effects:
 *      May reallocate st->resume and relocate queued entries. Updates
 *      st->cap, st->head, and st->tail while preserving st->count and
 *      the ring invariant:
 *
 *          tail == (head + count) % cap
 *
 *----------------------------------------------------------------------
 */
static int
resume_grow(SharedState *st)
{
    const size_t oldcap = st->cap;
    const size_t ncap   = oldcap > 0u ? oldcap * 2u : 32u;
    int64_t     *n      = ns_realloc(st->resume,
                                    ncap * sizeof(int64_t));

    if (n == NULL) {
        return -1;
    }

    if (st->count > 0u) {
        assert(st->count == oldcap);
        if (st->head != 0u) {
            /*
             * A full wrapped ring has tail == head. Move both portions
             * to the upper half of the enlarged allocation, preserving
             * their logical FIFO order.
             */
            const size_t upper = oldcap - st->head;

            assert(st->tail == st->head);

            memmove(n + (ncap - upper),
                    n + st->head,
                    upper * sizeof(int64_t));
            memmove(n + (ncap - st->count),
                    n,
                    st->tail * sizeof(int64_t));

            st->head = ncap - st->count;
            st->tail = 0u;
        } else {
            /*
             * The full ring was already linear at the start of the
             * allocation. Its next free slot is immediately after the
             * existing entries in the enlarged ring.
             */
            st->tail = st->count;
        }
    } else {
        st->head = 0u;
        st->tail = 0u;
    }

    st->resume = n;
    st->cap    = ncap;
    assert(st->tail == (st->head + st->count) % st->cap);

    return 0;
}

/*======================================================================
 * Function Implementations: SharedState
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 * SharedStateInit --
 *
 *      Initialize a caller-provided SharedState to a clean baseline.
 *      All counters and the ring buffer pointer are zeroed; the internal
 *      mutex is created via Ns_MutexInit(). The optional wake callback
 *      and its argument are recorded for later use by the owner.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes st->lock, writes wake_cb and wake_arg, and zeros:
 *      st->resume, st->cap, st->count, st->head, st->tail. No memory
 *      allocations are performed here. Must be called exactly once
 *      before any concurrent use of *st.
 *
 *----------------------------------------------------------------------
 */
void SharedStateInit(SharedState *st, SharedWakeFn wake_cb, void *wake_arg) {
    memset(st, 0, sizeof(*st));

    Ns_AtomicUint32Init(&st->resume_pending, 0u);
    st->wake_cb  = wake_cb;
    st->wake_arg = wake_arg;
}

/*
 *----------------------------------------------------------------------
 * SharedStateDestroy --
 *
 *      Tear down transient resources owned by *st*. Frees the ring
 *      buffer, if any, and resets capacity/counters to zero. The
 *      structure itself, its mutex, and callback fields are left
 *      intact for possible reuse or separate disposal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls ns_free(st->resume) (safe when NULL) and sets st->resume
 *      to NULL; clears st->cap, st->count, st->head, and st->tail to
 *      0.  Does not destroy st->lock and does not free 'st' or
 *      'wake_arg'.  The caller must ensure no concurrent access (by
 *      holding st->lock or after global quiescence).
 *
 *----------------------------------------------------------------------
 */
void SharedStateDestroy(SharedState *st) {
    ns_free(st->resume);
    st->resume = NULL;
    st->cap = st->count = st->head = st->tail = 0;
}

/*======================================================================
 * Function Implementations: SharedStream
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 * SharedStreamInit --
 *
 *      Initialize a caller-provided SharedStream to a clean baseline.
 *      Zeros all fields, initializes the per-stream mutex, records the
 *      owning SharedState, and stores a stream-id hint for diagnostics
 *      or deferred binding.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes ss->lock; writes ss->st and ss->sid_hint; leaves the
 *      tx_queued/tx_pending chunk queues zeroed (no allocations here).
 *      Must be called exactly once before any concurrent use of *ss*.
 *      The caller must ensure that *owner* outlives *ss* or otherwise
 *      coordinates teardown.
 *
 *----------------------------------------------------------------------
 */
void SharedStreamInit(SharedStream *ss, SharedState *owner, int64_t sid) {
    memset(ss, 0, sizeof(*ss));

    Ns_AtomicUint32Init(&ss->hdrs_ready, 0u);
    Ns_AtomicUint32Init(&ss->resume_enqueued, 0u);
    Ns_AtomicUint32Init(&ss->tx_state, 0u);

    ss->st       = owner;
    ss->sid_hint = sid;
    /* queues and mutexes already zeroed */
}

/*
 *----------------------------------------------------------------------
 * SharedStreamDestroy --
 *
 *      Teardown helper: clear TX queues under the stream lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Acquires ss->lock; calls ChunkQueueClear() on tx_queued/pending,
 *      releasing any queued buffers. Does not destroy ss->lock or free ss;
 *      caller must ensure no concurrent users.
 *----------------------------------------------------------------------
 */
void SharedStreamDestroy(SharedStream *ss) {
    SharedCounterStatsIncr(&sharedCounterStats.counters.stream_destroy_lock);
    Ns_MutexLock(&ss->lock);
    ChunkQueueClear(&ss->tx_queued);
    ChunkQueueClear(&ss->tx_pending);
    Ns_MutexUnlock(&ss->lock);
}

/*======================================================================
 * Function Implementations: Headers readiness
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * SharedHdrsIsReady --
 *
 *      Test whether completed response-header fields have been published
 *      for consumption by the QUIC thread.
 *
 *      When the acquire load observes the value stored by
 *      SharedHdrsSetReady(), the associated response-header fields written
 *      before that release store are visible to the calling thread.
 *
 * Results:
 *      Nonzero if headers are ready; 0 otherwise.
 *
 * Side effects:
 *      None for native atomic implementations. On platforms using the
 *      atomic fallback implementation, temporarily acquires the atomic
 *      object's internal mutex.
 *
 *----------------------------------------------------------------------
 */
int  SharedHdrsIsReady(SharedStream *ss) {
    /*
     * Acquire the response-header fields published by
     * SharedHdrsSetReady().
     */
    return Ns_AtomicUint32LoadAcquire(&ss->hdrs_ready) != 0u;
}

/*
 *----------------------------------------------------------------------
 *
 * SharedHdrsSetReady --
 *
 *      Publish the completed response-header fields and mark them ready
 *      for consumption by the QUIC thread.
 *
 *      The release store ensures that writes performed by the producer
 *      before this call become visible to a consumer that observes the
 *      flag through SharedHdrsIsReady().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Atomically sets ss->hdrs_ready to 1 with release ordering. On
 *      platforms using the atomic fallback implementation, temporarily
 *      acquires the atomic object's internal mutex. Idempotent; performs
 *      no I/O or allocation.
 *
 *----------------------------------------------------------------------
 */
void SharedHdrsSetReady(SharedStream *ss) {
    /*
     * Publish the completed response-header fields to the QUIC thread.
     */
    Ns_AtomicUint32StoreRelease(&ss->hdrs_ready, 1u);
}

/*
 *----------------------------------------------------------------------
 *
 * SharedHdrsClear --
 *
 *      Clear the atomic header-readiness flag after the staged response
 *      headers have been consumed or discarded.
 *
 *      Relaxed ordering is sufficient because clearing the flag does not
 *      publish associated shared state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Atomically sets ss->hdrs_ready to 0. On platforms using the atomic
 *      fallback implementation, temporarily acquires the atomic object's
 *      internal mutex. Idempotent; performs no I/O or allocation.
 *
 *----------------------------------------------------------------------
 */
void SharedHdrsClear(SharedStream *ss) {
    /*
     * Clearing the notification does not publish associated data.
     */
    Ns_AtomicUint32StoreRelaxed(&ss->hdrs_ready, 0u);
}

/*======================================================================
 * Function Implementations: Body enqueue / EOF
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 * SharedEnqueueBody --
 *
 *      Enqueue a payload into the stream TX queue (thread-safe); does
 *      not issue a resume tick.
 *
 * Results:
 *      Returns len on success; 0 if buf is NULL or len == 0.
 *
 * Side effects:
 *      Allocates a Chunk from (buf,len); acquires ss->lock; appends to
 *      ss->tx_queued with label (default "enqueue"); logs at Notice with
 *      ss->sid_hint and projected queued bytes. Caller must trigger
 *      SharedRequestResume() for this SID if needed.
 *----------------------------------------------------------------------
 */
size_t SharedEnqueueBody(SharedStream *ss, const void *buf, size_t len, const char *label) {
  Chunk *ch;

  if (!buf || len == 0) {
    return 0;
  }

  Ns_Log(Ns_LogQuicDebug,
         "H3[%lld] SharedEnqueueBody: +%zu",
         (long long)ss->sid_hint,
         len);

  ch = ChunkInit(buf, len);

  SharedCounterStatsIncr(&sharedCounterStats.counters.stream_enqueue_lock);

  Ns_MutexLock(&ss->lock);

  ChunkEnqueue(&ss->tx_queued, ch, label ? label : "enqueue");
  (void)Ns_AtomicUint32FetchOrRelease(&ss->tx_state, SHARED_TX_QUEUED);
  Ns_MutexUnlock(&ss->lock);

  return len;
}

/*
 *----------------------------------------------------------------------
 * SharedMarkClosedByApp --
 *
 *      Mark the stream as closed by the application (thread-safe).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Acquires ss->lock and sets SHARED_TX_CLOSED in ss->tx_state.
 *      The operation is idempotent and does not trigger a wake or resume
 *      request.
 *----------------------------------------------------------------------
 */
void SharedMarkClosedByApp(SharedStream *ss) {
    (void)Ns_AtomicUint32FetchOrRelease(&ss->tx_state, SHARED_TX_CLOSED);
}

/*======================================================================
 * Function Implementations: Body helpers for consumer
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 * SharedSpliceQueuedToPending --
 *
 *      Thread-safe splice of up to maxbytes from ss->tx_queued to
 *      ss->tx_pending, preserving FIFO order.
 *
 * Results:
 *      Returns the number of bytes moved (<= maxbytes).
 *
 * Side effects:
 *      Acquires ss->lock; relinks chunks between queues and updates
 *      counters. No logging or resume/wake is triggered.
 *----------------------------------------------------------------------
 */
size_t
SharedSpliceQueuedToPending(SharedStream *ss, size_t maxbytes)
{
    size_t moved;

    SharedCounterStatsIncr(&sharedCounterStats.counters.stream_splice_lock);

    Ns_MutexLock(&ss->lock);

    moved = ChunkQueueMove(&ss->tx_queued,
                           &ss->tx_pending,
                           maxbytes);

    if (ss->tx_queued.unread == 0u) {
        /*
         * Clear only the queued bit while preserving a concurrent
         * SHARED_TX_CLOSED transition.
         */
        (void)Ns_AtomicUint32FetchAndRelease(&ss->tx_state, ~SHARED_TX_QUEUED);
    }

    Ns_MutexUnlock(&ss->lock);

    return moved;
}

/*
 *----------------------------------------------------------------------
 * SharedTrimPending --
 *
 *      Consume up to nbytes from the pending TX queue (thread-safe),
 *      delegating to ChunkQueueTrim(); when drain is true, it may drop
 *      fully consumed chunks per ChunkQueueTrim semantics.
 *
 * Results:
 *      Number of bytes actually trimmed (<= nbytes).
 *
 * Side effects:
 *      Acquires ss->lock; mutates ss->tx_pending counters/links.
 *      Emits Notice logs with before/after unread byte counts.
 *      Does not trigger wake/resume.
 *----------------------------------------------------------------------
 */
size_t SharedTrimPending(SharedStream *ss, size_t nbytes, bool drain) {
    size_t n;

    Ns_Log(Ns_LogQuicDebug,
           "SharedTrimPending (%zu bytes): before trim unread %" PRIuz,
           nbytes,
           ss->tx_pending.unread);

    n = ChunkQueueTrim(&ss->tx_pending, nbytes, drain);

    Ns_Log(Ns_LogQuicDebug,
           "SharedTrimPending (%zu bytes): after trim unread %" PRIuz,
           nbytes,
           ss->tx_pending.unread);
    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * SharedTrimPendingFromVec --
 *
 *      Trim body bytes from the consumer-owned pending queue that overlap
 *      the span [base, base + len). Trimming proceeds only when base points
 *      into the current head chunk; otherwise, the span is assumed to
 *      contain framing or header data and nothing is removed.
 *
 *      The pending queue is owned exclusively by the H3/QUIC thread.
 *      Once chunks have been transferred from tx_queued to tx_pending,
 *      the producer must neither inspect nor modify them. The caller must
 *      therefore execute with the owning connection's thread affinity.
 *
 * Results:
 *      Returns the number of payload bytes trimmed, which is at most len.
 *
 * Side effects:
 *      Advances chunk data pointers, updates chunk lengths and the pending
 *      unread-byte count, and may free fully consumed chunks. Maintains
 *      consistent queue head and tail pointers. Does not acquire ss->lock
 *      and does not trigger a wake or resume request.
 *
 *----------------------------------------------------------------------
 */
size_t
SharedTrimPendingFromVec(SharedStream *ss, const uint8_t *base, size_t len)
{
    const size_t requested = len;
    size_t       trimmed   = 0u;

    /*
     * The caller must execute on the owning H3/QUIC thread. The caller's
     * ConnCtx affinity assertion enforces this requirement in debug builds.
     *
     * No mutex is required: tx_pending is owned by the QUIC thread.
     */
    while (len > 0u && ss->tx_pending.head != NULL) {
        Chunk          *ch = ss->tx_pending.head;
        const uintptr_t b  = (uintptr_t)base;
        const uintptr_t p  = (uintptr_t)ch->p;
        size_t          off;
        size_t          room;
        size_t          take;
        size_t          consumed;

        /*
         * A relational comparison between pointers belonging to different
         * allocations is undefined in C. Compare their integer
         * representations and avoid overflow in p + ch->len.
         */
        if (b < p || b - p >= ch->len) {
            break;              /* framing or headers, not body data */
        }

        off      = (size_t)(b - p);
        room     = ch->len - off;
        take     = len < room ? len : room;
        consumed = off + take;

        ch->p   += consumed;
        ch->len -= consumed;

        assert(ss->tx_pending.unread >= consumed);

        if (ss->tx_pending.unread >= consumed) {
            ss->tx_pending.unread -= consumed;
        } else {
            ss->tx_pending.unread = 0u;
        }

        trimmed += take;
        len     -= take;

        if (ch->len != 0u) {
            break;              /* write ended inside this chunk */
        }

        ss->tx_pending.head = ch->next;
        if (ss->tx_pending.head == NULL) {
            ss->tx_pending.tail = NULL;
        }
        ns_free(ch);

        /*
         * Continue at the beginning of the next pending chunk. This
         * preserves the behavior of the current implementation when the
         * reported write spans multiple body chunks.
         */
        if (ss->tx_pending.head != NULL) {
            base = ss->tx_pending.head->p;
        }
    }

    Ns_Log(Ns_LogQuicDebug,
           "H3[%lld] SharedTrimPendingFromVec:"
           " requested %zu, trimmed %zu, pending %zu",
           (long long)ss->sid_hint,
           requested,
           trimmed,
           ss->tx_pending.unread);

    return trimmed;
}

/*
 *----------------------------------------------------------------------
 *
 * SharedPendingUnreadBytes --
 *
 *      Return the unread-byte count of the consumer-owned pending TX
 *      queue.
 *
 *      The pending queue is owned exclusively by the H3/QUIC thread.
 *      The caller must therefore execute with the owning connection's
 *      thread affinity.
 *
 * Results:
 *      Returns ss->tx_pending.unread.
 *
 * Side effects:
 *      None. Does not acquire ss->lock, allocate memory, or trigger a
 *      wake or resume request.
 *
 *----------------------------------------------------------------------
 */
size_t SharedPendingUnreadBytes(SharedStream *ss) {
    return ss->tx_pending.unread;
}

/*
 *----------------------------------------------------------------------
 * SharedBuildVecsFromPending --
 *
 *      Build (not snapshot) an array of nghttp3_vec from the pending TX
 *      queue: copies pointers/lengths only, preserving FIFO order and
 *      without mutating the queue. See: https://nghttp2.org/nghttp3/
 *
 * Results:
 *      Number of vectors written (<= veccnt); 0 if vecs is NULL, veccnt
 *      is 0, or no pending data.
 *
 * Side effects:
 *      Copies pointers and lengths from the pending queue into vecs.
 *      Does not modify the queue, acquire ss->lock, allocate memory,
 *      or trigger a wake or resume request.
 *
 * Caller requirements:
 *      Must execute on the owning H3/QUIC thread. The returned pointers
 *      remain valid until that thread advances or trims tx_pending.
 *
 *----------------------------------------------------------------------
 */
size_t
SharedBuildVecsFromPending(SharedStream *ss,
                           nghttp3_vec *vecs,
                           size_t veccnt)
{
    size_t out = 0u;

    if (vecs == NULL || veccnt == 0u) {
        return 0u;
    }

    /*
     * No mutex is required: tx_pending is owned exclusively by the
     * H3/QUIC thread. The caller asserts the owning ConnCtx affinity.
     */
    for (Chunk *ch = ss->tx_pending.head;
         ch != NULL && out < veccnt;
         ch = ch->next) {
        Ns_Log(Ns_LogQuicDebug,
               "H3[%lld] SharedBuildVecsFromPending"
               " appending chunk len %zu",
               (long long)ss->sid_hint,
               ch->len);

        vecs[out].base = ch->p;
        vecs[out].len  = ch->len;
        out++;
    }

    return out;
}

/*======================================================================
 * Function Implementations: Resume ring
 *
 *      The "resume ring" is a lock-protected circular FIFO of int64_t
 *      SIDs in SharedState, used to coalesce runnable streams: each
 *      stream sets a resume_enqueued flag so duplicates are
 *      avoided. The ring stores only SIDs (no pointers); capacity may
 *      grow (see resume_grow).
 *======================================================================
 */
/*
 *----------------------------------------------------------------------
 *
 * resume_push_unlocked --
 *
 *      Append a stream ID to the resume ring. If the ring is full,
 *      enlarge it before inserting the new entry.
 *
 *      The caller must hold st->lock. This function does not inspect or
 *      modify the per-stream resume_enqueued flag; the caller must set
 *      that flag only after this function succeeds.
 *
 * Results:
 *      NS_TRUE when the stream ID was inserted successfully.
 *      NS_FALSE when the ring could not be enlarged and no entry was
 *      inserted.
 *
 * Side effects:
 *      May grow and rearrange the resume ring. On success, stores sid at
 *      tail, advances tail with wrap-around, and increments count. On
 *      failure, leaves the logical ring contents unchanged.
 *
 *----------------------------------------------------------------------
 */
static bool
resume_push_unlocked(SharedState *st, int64_t sid)
{
    if (st->count == st->cap && resume_grow(st) != 0) {
        return NS_FALSE;
    }

    st->resume[st->tail] = sid;
    st->tail = (st->tail + 1u) % st->cap;
    st->count++;

    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * SharedRequestResume --
 *
 *      Enqueue a stream for resumption and (edge-triggered) wake the
 *      worker when transitioning from idle.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Under st->lock, checks whether ss->resume_enqueued is already
 *      set. If not, attempts to append sid to the resume ring. Only
 *      after successful insertion does it set resume_enqueued and
 *      request a wake when the ring transitioned from empty to
 *      nonempty.
 *
 *      Outside the lock, invokes st->wake_cb(st->wake_arg) when a wake
 *      is required. Ring growth may allocate memory. If growth fails,
 *      the SID is not inserted, resume_enqueued remains clear, and no
 *      wake is issued.
 *
 *----------------------------------------------------------------------
 */
void
SharedRequestResume(SharedState *st, SharedStream *ss, int64_t sid)
{
    bool need_wake = NS_FALSE;

    /*
     * Claim the per-stream resume notification. If it was already set,
     * an entry is pending or currently being consumed, so this request
     * is coalesced with that processing pass.
     */
    if (Ns_AtomicUint32ExchangeRelaxed(&ss->resume_enqueued, 1u) != 0u) {
        return;
    }
    SharedCounterStatsIncr(&sharedCounterStats.counters.resume_request_lock);

    Ns_MutexLock(&st->lock);

    if (st->resume_stopped) {
        Ns_AtomicUint32StoreRelaxed(&ss->resume_enqueued, 0u);

    } else {
        const bool was_empty = (st->count == 0u);

        if (resume_push_unlocked(st, sid)) {
            if (was_empty) {
                /*
                 * Publish the transition from an empty to a nonempty
                 * resume ring before releasing the ring lock.
                 */
                Ns_AtomicUint32StoreRelease(&st->resume_pending, 1u);
                need_wake = NS_TRUE;
            }
        } else {
            /*
             * No ring entry was created. Release the claim so a later
             * request can try again.
             */
            Ns_AtomicUint32StoreRelaxed(&ss->resume_enqueued, 0u);
        }
    }
    Ns_MutexUnlock(&st->lock);

    if (need_wake && st->wake_cb != NULL) {
        st->wake_cb(st->wake_arg);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SharedResumeClear --
 *
 *      Clear the per-stream resume-enqueued flag so the stream can be
 *      placed on the resume ring again after its current entry has been
 *      consumed.
 *
 *      The flag is cleared before processing the consumed resume entry.
 *      A concurrent producer either observes the previous set value and
 *      coalesces its request with the current processing pass, or observes
 *      zero and enqueues a new resume entry.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Atomically sets ss->resume_enqueued to zero using relaxed ordering.
 *      On platforms using the atomic fallback implementation, temporarily
 *      acquires the atomic object's internal mutex.
 *
 *----------------------------------------------------------------------
 */
void SharedResumeClear(SharedStream *ss) {
    Ns_AtomicUint32StoreRelaxed(&ss->resume_enqueued, 0u);
}

/*
 *----------------------------------------------------------------------
 *
 * SharedDrainResume --
 *
 *      Drain up to 'cap' stream IDs from the connection-level resume
 *      ring into 'out'. Entries are removed in FIFO order.
 *
 *      When the ring becomes empty, clear resume_pending while holding
 *      the ring lock. This also repairs an inconsistent state in which
 *      resume_pending is set although the ring was already empty on
 *      entry.
 *
 * Results:
 *
 *      Returns the number of stream IDs written to 'out', in the range
 *      0..cap. The arguments 'out' and 'cap' must specify nonempty
 *      output storage.
 *
 * Side effects:
 *
 *      Acquires st->lock, advances the ring head and decrements its
 *      entry count. When no entries remain, clears resume_pending.
 *
 *      If a stale resume_pending flag is detected without draining an
 *      entry, logs a warning containing the drain capacity, ring
 *      capacity, head and tail positions. Per-stream resume_enqueued
 *      flags are not cleared here and must be handled separately by
 *      the consumer of the returned stream IDs.
 *
 *----------------------------------------------------------------------
 */
size_t
SharedDrainResume(SharedState *st, int64_t *out, size_t cap)
{
    size_t n = 0u, head = 0u, tail = 0u;
    bool   stale_pending = NS_FALSE;

    if (out == NULL || cap == 0u) {
        return 0u;
    }
    SharedCounterStatsIncr(&sharedCounterStats.counters.resume_drain_lock);

    Ns_MutexLock(&st->lock);

    while (n < cap && st->count > 0u) {
        out[n++] = st->resume[st->head];
        st->head = (st->head + 1u) % st->cap;
        st->count--;
    }

    if (st->count == 0u) {
        head = st->head;
        tail = st->tail;
        stale_pending = (n == 0u && Ns_AtomicUint32LoadRelaxed(&st->resume_pending) != 0u);

        Ns_AtomicUint32StoreRelaxed(&st->resume_pending, 0u);
    }
    Ns_MutexUnlock(&st->lock);

    if (unlikely(stale_pending)) {
        Ns_Log(Warning,
               "H3 resume ring: repaired stale pending flag "
               "(drained %zu, drain capacity %zu, ring capacity %zu, head %zu, tail %zu)",
               n, cap, st->cap, head, tail);
    }

    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * SharedResumeStop --
 *
 *      Permanently stop resume scheduling for this shared connection
 *      state. This operation is intended for terminal connection
 *      shutdown and must not be reversed.
 *
 * Results:
 *
 *      None.
 *
 * Side effects:
 *
 *      Acquires st->lock, prevents subsequent resume requests from
 *      entering the ring, discards all queued stream IDs and clears the
 *      resume_pending summary flag. The allocated ring storage is
 *      retained until SharedStateDestroy().
 *
 *      Per-stream resume_enqueued flags belonging to entries already in
 *      the ring are not cleared. They are irrelevant after this terminal
 *      transition and disappear when the streams are destroyed.
 *
 *----------------------------------------------------------------------
 */
void
SharedResumeStop(SharedState *st)
{
    Ns_MutexLock(&st->lock);

    st->resume_stopped = NS_TRUE;
    st->head = 0u;
    st->tail = 0u;
    st->count = 0u;
    Ns_AtomicUint32StoreRelaxed(&st->resume_pending, 0u);

    Ns_MutexUnlock(&st->lock);
}

#endif /* HAVE_NGHTTP3 */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
