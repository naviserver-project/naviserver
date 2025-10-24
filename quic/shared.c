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
 * - Per-stream data (queues, hdrs flags, closed_by_app) is protected by
 *   ss->lock.
 * - The global resume ring is protected by st->lock.
 * - The resume_enqueued flag is set under st->lock in the enqueue path,
 *   and cleared under ss->lock by the consumer after the SID is serviced.
 *   This avoids duplicate enqueues while minimizing cross-lock holding.
 * - Callers must not hold ss->lock while performing potentially blocking
 *   I/O; build vecs under the lock, then write, then trim under the lock.
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


#include "../include/ns.h"
#include "shared.h"


#include "shared.h"
#include "ns.h" /* Ns_Mutex*, Ns_Log */
#include <string.h>
#include <stdlib.h>

/* ---------- Prototypes ---------- */
static int resume_grow(SharedState *st) NS_GNUC_NONNULL(1);


/*======================================================================
 * Function Implementations: Utilities
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * resume_grow --
 *
 *      Double the capacity of the resume ring buffer in SharedState,
 *      reallocating its storage and re-laying out existing entries
 *      into a contiguous linear form if the buffer was wrapped.
 *
 * Results:
 *      0 on success, -1 on memory allocation failure.
 *
 * Side effects:
 *      Updates st->resume, st->cap, st->head, and st->tail in place.
 *      May move existing elements if the ring buffer was wrapped.
 *
 *----------------------------------------------------------------------
 */
static int resume_grow(SharedState *st) {
    size_t   ncap = st->cap ? st->cap * 2 : 32;
    int64_t *n = ns_realloc(st->resume, ncap * sizeof(int64_t));

    if (n == NULL) {
      return -1;
    }

    /* Rebuild as linear head..tail if wrapped */
    if (st->count && st->head != 0) {
        /* move [head..cap) to end of new */
        size_t tail_part = st->cap - st->head;
        memmove(n + (ncap - tail_part), n + st->head, tail_part * sizeof(int64_t));
        /* slide [0..tail) to after that */
        memmove(n + (ncap - tail_part - st->tail), n, st->tail * sizeof(int64_t));
        st->head = ncap - st->count;
        st->tail = 0;
    }

    st->resume = n;
    st->cap    = ncap;
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
 *      Initializes st->lock, writes wake_cb and wake_arg, and zeroes:
 *      st->resume, st->cap, st->count, st->head, st->tail. No memory
 *      allocations are performed here. Must be called exactly once
 *      before any concurrent use of *st.
 *
 *----------------------------------------------------------------------
 */
void SharedStateInit(SharedState *st, SharedWakeFn wake_cb, void *wake_arg) {
    memset(st, 0, sizeof(*st));
    Ns_MutexInit(&st->lock);
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
    Ns_MutexInit(&ss->lock);
    ss->st       = owner;
    ss->sid_hint = sid;
    /* queues already zeroed */
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
 * SharedHdrsIsReady --
 *
 *      Thread-safe read of ss->hdrs_ready.
 *
 * Results:
 *      Nonzero if headers are ready; 0 otherwise.
 *
 * Side effects:
 *      Temporarily acquires ss->lock.
 *----------------------------------------------------------------------
 */
int  SharedHdrsIsReady(SharedStream *ss) {
    int v;
    Ns_MutexLock(&ss->lock);
    v = ss->hdrs_ready;
    Ns_MutexUnlock(&ss->lock);
    return v;
}

/*
 *----------------------------------------------------------------------
 * SharedHdrsSetReady --
 *
 *      Mark headers as ready in a thread-safe manner.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Acquires ss->lock and sets ss->hdrs_ready = 1. Idempotent; no
 *      allocations or logging.
 *----------------------------------------------------------------------
 */
void SharedHdrsSetReady(SharedStream *ss) {
    Ns_MutexLock(&ss->lock);
    ss->hdrs_ready = 1;
    Ns_MutexUnlock(&ss->lock);
}

/*
 *----------------------------------------------------------------------
 * SharedHdrsClear --
 *
 *      Clear the header-ready flag in a thread-safe manner.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Acquires ss->lock and sets ss->hdrs_ready = 0. Idempotent; no I/O
 *      or allocations.
 *----------------------------------------------------------------------
 */
void SharedHdrsClear(SharedStream *ss) {
    Ns_MutexLock(&ss->lock);
    ss->hdrs_ready = 0;
    Ns_MutexUnlock(&ss->lock);
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

  Ns_Log(Notice, "H3[%lld] SharedEnqueueBody: +%zu (queued=%zu)",
       (long long)ss->sid_hint, len, ss->tx_queued.unread + len);

  ch = ChunkInit(buf, len);
  Ns_MutexLock(&ss->lock);
  ChunkEnqueue(&ss->tx_queued, ch, label ? label : "enqueue");
  Ns_MutexUnlock(&ss->lock);
  /* NOTE: we do NOT push resume here; caller should call SharedRequestResume() for this SID */
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
 *      Acquires ss->lock and sets ss->closed_by_app = NS_TRUE. Idempotent;
 *      no logging or wake/resume is triggered.
 *----------------------------------------------------------------------
 */
void SharedMarkClosedByApp(SharedStream *ss) {
    Ns_MutexLock(&ss->lock);
    ss->closed_by_app = NS_TRUE;
    Ns_MutexUnlock(&ss->lock);
}

/*======================================================================
 * Function Implementations: Body helpers for consumer
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 * SharedTxReadable --
 *
 *      Thread-safe predicate: returns whether TX has unread bytes.
 *
 * Results:
 *      Nonzero if ss->tx_queued.unread > 0; 0 otherwise.
 *
 * Side effects:
 *      Temporarily acquires ss->lock.
 *----------------------------------------------------------------------
 */
int SharedTxReadable(SharedStream *ss) {
    int v;
    Ns_MutexLock(&ss->lock);
    v = (ss->tx_queued.unread > 0);
    Ns_MutexUnlock(&ss->lock);
    return v;
}

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
size_t SharedSpliceQueuedToPending(SharedStream *ss, size_t maxbytes) {
    size_t moved;
    Ns_MutexLock(&ss->lock);
    moved = ChunkQueueMove(&ss->tx_queued, &ss->tx_pending, maxbytes);
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

    Ns_Log(Notice, "SharedTrimPending (%ld bytes): before ChunkQueueTrim unread %ld", nbytes, ss->tx_pending.unread);

    Ns_MutexLock(&ss->lock);
    n = ChunkQueueTrim(&ss->tx_pending, nbytes, drain);
    Ns_MutexUnlock(&ss->lock);

    Ns_Log(Notice, "SharedTrimPending (%ld bytes): after ChunkQueueTrim unread %ld", nbytes, ss->tx_pending.unread);

    return n;
}

/*
 *----------------------------------------------------------------------
 * SharedTrimPendingFromVec --
 *
 *      Trim body bytes from ss->tx_pending that overlap the span
 *      [base, base+len). Only proceeds when 'base' points inside the
 *      current head chunk; otherwise nothing is removed (assumed to be
 *      framing/headers). Preserves FIFO order while possibly freeing
 *      fully consumed chunks.
 *
 * Results:
 *      Returns the number of payload bytes actually trimmed (<= len).
 *
 * Side effects:
 *      Acquires ss->lock; mutates head chunk pointers (p/len), updates
 *      ss->tx_pending.unread, may free exhausted chunks, and leaves the
 *      queue head/tail consistent. Emits Notice logs before/after with
 *      unread and trimmed counts. No wake/resume is triggered.
 *----------------------------------------------------------------------
 */
size_t SharedTrimPendingFromVec(SharedStream *ss, const uint8_t *base, size_t len) {
    size_t trimmed = 0;

    Ns_Log(Notice, "H3[%lld] SharedTrimPendingFromVec (%ld bytes): before ChunkQueueTrim unread %ld",
            (long long)ss->sid_hint, len, ss->tx_pending.unread);

    Ns_MutexLock(&ss->lock);

    while (len && ss->tx_pending.head) {
        Chunk *ch = ss->tx_pending.head;

        // Only trim if this vec starts inside the chunk
        if (base < ch->p || base >= ch->p + ch->len) {
            break; // not body data (must be framing/headers)
        } else {
          size_t off  = (size_t)(base - ch->p);
          size_t room = ch->len - off;
          size_t take = len < room ? len : room;

          // advance inside this chunk
          ch->p   += off + take;
          ch->len -= off + take;

          // keep unread honest
          if (ss->tx_pending.unread >= off + take) {
            ss->tx_pending.unread -= (off + take);
          } else {
            ss->tx_pending.unread = 0; // defensive
          }

          trimmed += take;
          len     -= take;

          // drop exhausted chunk
          if (ch->len == 0) {
            ss->tx_pending.head = ch->next;
            if (!ss->tx_pending.head) ss->tx_pending.tail = NULL;
            ns_free(ch);
          } else {
            // vec ended inside this chunk
            break;
          }

          // next loop continues with new head, base already accounted
          base = ss->tx_pending.head ? ss->tx_pending.head->p : base;
        }
    }

    Ns_MutexUnlock(&ss->lock);

    Ns_Log(Notice, "H3[%lld] SharedTrimPendingFromVec (%ld bytes): after ChunkQueueTrim unread %ld (trimmed %ld)",
            (long long)ss->sid_hint, len, ss->tx_pending.unread, trimmed);

    return trimmed;
}

/*
 *----------------------------------------------------------------------
 * SharedPendingUnreadBytes / SharedQueuedUnreadBytes --
 *
 *      Thread-safe accessors for unread byte counters in the TX queues:
 *      'Pending' reports ss->tx_pending.unread; 'Queued' reports
 *      ss->tx_queued.unread.
 *
 * Results:
 *      Returns the number of unread bytes for the respective queue.
 *
 * Side effects:
 *      Temporarily acquires ss->lock; no allocation or logging.
 *
 *----------------------------------------------------------------------
 */
size_t SharedPendingUnreadBytes(SharedStream *ss) {
    size_t n;
    Ns_MutexLock(&ss->lock);
    n = ss->tx_pending.unread;
    Ns_MutexUnlock(&ss->lock);
    return n;
}
size_t SharedQueuedUnreadBytes(SharedStream *ss) {
    size_t n;
    Ns_MutexLock(&ss->lock);
    n = ss->tx_queued.unread;
    Ns_MutexUnlock(&ss->lock);
    return n;
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
 *      Acquires ss->lock; iterates ss->tx_pending; logs each appended
 *      chunk at Notice. No allocations; no wake/resume. Callers must
 *      trim after actual writes (e.g., SharedTrimPendingFromVec or
 *      SharedTrimPending) and ensure the queue is not mutated while the
 *      produced vecs are in use.
 *
 *----------------------------------------------------------------------
 */
size_t SharedBuildVecsFromPending(SharedStream *ss, nghttp3_vec *vecs, size_t veccnt) {
    size_t out = 0;

    if (vecs == NULL || veccnt == 0) {
        return 0;
    }

    Ns_MutexLock(&ss->lock);
    for (Chunk *ch = ss->tx_pending.head; ch && out < veccnt; ch = ch->next) {
      Ns_Log(Notice, "H3[%lld] SharedBuildVecsFromPending appending chunk len %ld",
             (long long)ss->sid_hint, ch->len);
        vecs[out].base = ch->p;
        vecs[out].len  = ch->len;
        out++;
    }
    Ns_MutexUnlock(&ss->lock);

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
static void resume_push_unlocked(SharedState *st, int64_t sid) {
    if (st->count == st->cap && resume_grow(st) != 0) {
        return; /* drop non-fatally */
    }
    st->resume[st->tail] = sid;
    st->tail = (st->tail + 1) % st->cap;
    st->count++;
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
 *      Under st->lock: if ss->resume_enqueued is false, sets it true
 *      and pushes 'sid' onto the ring; sets need_wake when the ring
 *      was previously empty (edge-triggered nudge). Outside the lock:
 *      calls st->wake_cb(st->wake_arg) if need_wake. May allocate
 *      when the ring grows. The consumer must clear the per-stream
 *      flag when the SID is popped/handled.
 *
 *----------------------------------------------------------------------
 */
void SharedRequestResume(SharedState *st, SharedStream *ss, int64_t sid) {
    int need_wake = 0;

    Ns_MutexLock(&st->lock);
    if (!ss->resume_enqueued) {
        ss->resume_enqueued = 1;
        resume_push_unlocked(st, sid);
        need_wake = (st->count == 1); /* edge: ring was empty before push */
    }
    Ns_MutexUnlock(&st->lock);

    if (need_wake && st->wake_cb) {
      st->wake_cb(st->wake_arg);   /* wake QUIC thread */
    }
}

#if 0
/*
 *----------------------------------------------------------------------
 *
 * SharedPopResume --
 *
 *      Pop the next SID from the resume ring (circular FIFO), if any.
 *      Thread-safe; preserves FIFO order.
 *
 * Results:
 *      1 on success (stores SID into *out_sid); 0 if the ring is empty
 *      (out_sid is left untouched). Requires out_sid != NULL.
 *
 * Side effects:
 *      Acquires st->lock; advances head with wrap-around and decrements
 *      count. No allocation, no logging.
 *
 *----------------------------------------------------------------------
 */
int SharedPopResume(SharedState *st, int64_t *out_sid) {
    int have = 0;
    Ns_MutexLock(&st->lock);
    if (st->count) {
        *out_sid = st->resume[st->head];
        st->head = (st->head + 1) % st->cap;
        st->count--;
        have = 1;
    }
    Ns_MutexUnlock(&st->lock);
    return have;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * SharedResumeClear --
 *
 *      Clear the per-stream resume_enqueued flag so the stream can be
 *      requeued on the resume ring after handling.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Acquires ss->lock and sets ss->resume_enqueued = 0. Idempotent;
 *      no logging or wake/resume is triggered.
 *
 *----------------------------------------------------------------------
 */
void SharedResumeClear(SharedStream *ss) {
    Ns_MutexLock(&ss->lock);
    ss->resume_enqueued = 0;
    Ns_MutexUnlock(&ss->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * SharedDrainResume --
 *
 *      Drain up to 'cap' SIDs from the resume ring (circular FIFO) into
 *      'out' without blocking beyond the mutex. Preserves FIFO order.
 *
 * Results:
 *      Returns the number of SIDs written (0..cap). Requires out != NULL
 *      and cap > 0.
 *
 * Side effects:
 *      Acquires st->lock; advances head with wrap-around and decrements
 *      count for each popped entry. No allocation or logging. Per-stream
 *      resume flags must be cleared by the consumer separately.
 *
 *----------------------------------------------------------------------
 */
size_t
SharedDrainResume(SharedState *st, int64_t *out, size_t cap)
{
    size_t n = 0;

    if (out == NULL || cap == 0) {
        return 0;
    }

    Ns_MutexLock(&st->lock);

    while (n < cap && st->count > 0) {
        out[n++]  = st->resume[st->head];
        st->head  = (st->head + 1) % st->cap;
        st->count--;
    }

    Ns_MutexUnlock(&st->lock);
    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * SharedSnapshotRead --
 *
 *      Populate 'out' with a consistent snapshot of selected stream
 *      state (queued/pending byte counts and closed_by_app). Uses the
 *      stream mutex to read an atomic view.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Acquires ss->lock; writes to *out. No allocation or logging; does
 *      not mutate ss or its queues. Requires 'out' != NULL.
 *
 *----------------------------------------------------------------------
 */
void SharedSnapshotRead(SharedStream *ss, SharedSnapshot *out)
{
    Ns_MutexLock(&ss->lock);
    out->queued_bytes   = ss->tx_queued.unread;
    out->pending_bytes  = ss->tx_pending.unread;
    out->closed_by_app  = ss->closed_by_app;
    Ns_MutexUnlock(&ss->lock);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
