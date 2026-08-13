/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
 * Copyright (C) 2025 Gustaf Neumann
 */

/* shared.h - safe cross-thread API for H3 stream state */

#ifndef H3_SHARED_H
# define H3_SHARED_H

# include "../nsd/nsatomic.h"

/* Shared, lock-safe primitives for H3 stream body submission and resume signaling.
 *
 * - Producers (writer/conn threads) call:
 *     SharedMarkHdrsReady(...), SharedEnqueue(...), SharedMarkEOF(...)
 *   These never call nghttp3 and are safe from non-H3 threads.
 *
 * - The H3 thread calls:
 *     SharedPopResumes(...), SharedMarkResumed(...), SharedSpliceToPending(...)
 *   It then calls nghttp3_conn_resume_stream(sid) for each popped SID and
 *   proceeds with SSL_handle_events()/write step.
 */

# include <nghttp3/nghttp3.h>
# include "chunk.h"

/*
 * values for SharedStream->tx_state
 */
#define SHARED_TX_QUEUED 0x01u
#define SHARED_TX_CLOSED 0x02u

# ifdef __cplusplus
extern "C" {
# endif

    /* Optional wake callback: called (unlocked) after enqueue/resume push. */
    typedef void (*SharedWakeFn)(void *arg);
    /* ===== Shared state (per-connection) ======================================= */

    typedef struct SharedState {
        Ns_Mutex     lock;

        /* ring of stream IDs that need nghttp3_conn_resume_stream() */
        int64_t     *resume;
        size_t       cap;
        size_t       head;
        size_t       tail;
        size_t       count;

        Ns_AtomicUint32 resume_pending;   /* lock-free mirror of count != 0 */

        SharedWakeFn wake_cb;   /* e.g., h3_wake */
        void        *wake_arg;  /* e.g., dc */
    } SharedState;

    /* ===== Shared snapshot (per-connection) ======================================= */

    typedef struct {
        size_t queued_bytes;   /* bytes still queued (not handed to reader) */
        size_t pending_bytes;  /* bytes handed to reader but not yet trimmed */
        bool   closed_by_app;  /* producer closed (final chunk queued/already consumed) */
    } SharedSnapshot; /* A consistent view of the producer buffers*/


    /* ===== Shared stream (per H3 request/response stream) ====================== */

    typedef struct SharedStream {
        /*
         * Protects producer-to-consumer publication:
         *
         *   - producer appends chunks to tx_queued;
         *   - producer sets SHARED_TX_CLOSED in tx_state;
         *   - QUIC thread removes chunks from tx_queued and transfers their
         *     ownership to tx_pending.
         *
         * The lock does not protect tx_pending. Once transferred, pending
         * chunks are owned exclusively by the QUIC thread.
         */
        Ns_Mutex    lock;

        /*
         * Lock-free predicate state:
         *
         * SHARED_TX_QUEUED:
         *     tx_queued contains data or may require a splice attempt.
         *
         * SHARED_TX_CLOSED:
         *     producer has permanently finished.
         */
        Ns_AtomicUint32 tx_state;

        /*
         * Producer/consumer queue. The producer appends under lock; the QUIC
         * thread removes or transfers chunks under the same lock.
         */
        ChunkQueue  tx_queued;

        /*
         * QUIC-thread-owned queue. Only the QUIC thread builds vectors from,
         * advances, trims, and frees pending chunks. No lock is required for
         * these operations.
         */
        ChunkQueue  tx_pending;

        /*
         * Cross-thread header publication flag. The producer release-stores
         * readiness after completing the response-header fields; the QUIC
         * thread acquire-loads it before accessing those fields.
         */
        Ns_AtomicUint32 hdrs_ready;

        /*
         * Atomic producer/consumer notification coalescing. A set value means
         * that a resume request is queued or currently being processed.
         */
        Ns_AtomicUint32 resume_enqueued;

        SharedState *st;           /* owning connection-level shared state */
        int64_t      sid_hint;     /* stream ID used for diagnostics */
    } SharedStream;


    /* ===== SharedState and SharedStream lifecycle ============================= */

    void SharedStateInit(SharedState *st, SharedWakeFn wake_cb, void *wake_arg);
    void SharedStateDestroy(SharedState *st);

    void SharedStreamInit(SharedStream *ss, SharedState *owner, int64_t sid);
    void SharedStreamDestroy(SharedStream *ss);

    /* --- Headers (signal only; header arrays remain in sc) --- */
    int  SharedHdrsIsReady(SharedStream *ss);
    void SharedHdrsSetReady(SharedStream *ss);
    void SharedHdrsClear(SharedStream *ss);

    /* --- Body enqueue/EOF from producer side --- */
    size_t SharedEnqueueBody(SharedStream *ss, const void *buf, size_t len, const char *label);
    void   SharedMarkClosedByApp(SharedStream *ss);

    /* --- Body helpers used by data_reader / writer --- */
    int    SharedTxReadable(SharedStream *ss);                 /* queued.unread > 0 */
    size_t SharedSpliceQueuedToPending(SharedStream *ss, size_t maxbytes);
    size_t SharedTrimPending(SharedStream *ss, size_t nbytes, bool drain);
    size_t SharedTrimPendingFromVec(SharedStream *ss, const uint8_t *base,size_t len) NS_GNUC_NONNULL(1,2);
    size_t SharedPendingUnreadBytes(SharedStream *ss);
    size_t SharedQueuedUnreadBytes(SharedStream *ss);
    size_t SharedBuildVecsFromPending(SharedStream *ss, nghttp3_vec *vecs, size_t veccnt);

    /* --- Connection-level resume ring --- */
    void   SharedRequestResume(SharedState *st, SharedStream *ss, int64_t sid);
    int    SharedPopResume(SharedState *st, int64_t *out_sid);
    void   SharedResumeClear(SharedStream *ss);
    size_t SharedDrainResume(SharedState *st, int64_t *out, size_t cap)  NS_GNUC_NONNULL(1);
    static bool SharedHasResumePending(SharedState *st) NS_GNUC_NONNULL(1);

    /*
     * Read a transmit-state snapshot. The caller must execute on the owning
     * H3/QUIC thread. The mutex synchronizes tx_queued and tx_state; thread
     * affinity protects tx_pending.
     */
    void SharedSnapshotRead(SharedStream *ss, SharedSnapshot *out) NS_GNUC_NONNULL(1,2);
    static inline SharedSnapshot SharedSnapshotInit(SharedStream *ss) NS_GNUC_NONNULL(1);

    /* Tiny helpers (header-only / static inline) */
    static inline bool SharedHasData(const SharedSnapshot *s) {
        return (s->queued_bytes + s->pending_bytes) > 0;
    }
    static inline bool SharedIsEmpty(const SharedSnapshot *s) {
        return (s->queued_bytes + s->pending_bytes) == 0;
    }
    static inline bool SharedCanMove(const SharedSnapshot *s) {
        return s->pending_bytes == 0 && s->queued_bytes > 0;
    }

    static inline bool SharedEOFReady(const SharedSnapshot *s) {
        /* nothing left AND app has closed */
        return s->closed_by_app && SharedIsEmpty(s);
    }

    static inline bool SharedHasResumePending(SharedState *st)
    {
        return Ns_AtomicUint32LoadAcquire(&st->resume_pending) != 0u;
    }

    static inline void
    SharedTxStateSetLocked(SharedStream *ss, uint32_t bits)
    {
        const uint32_t state =
            Ns_AtomicUint32LoadRelaxed(&ss->tx_state);

        Ns_AtomicUint32StoreRelease(&ss->tx_state, state | bits);
    }

    static inline void
    SharedTxStateClearLocked(SharedStream *ss, uint32_t bits)
    {
        const uint32_t state =
            Ns_AtomicUint32LoadRelaxed(&ss->tx_state);

        Ns_AtomicUint32StoreRelease(&ss->tx_state, state & ~bits);
    }

    static inline SharedSnapshot SharedSnapshotInit(SharedStream *ss)  {
        SharedSnapshot snap;
        SharedSnapshotRead(ss, &snap);
        return snap;
    }


# ifdef __cplusplus
}
# endif

#endif /* H3_SHARED_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
