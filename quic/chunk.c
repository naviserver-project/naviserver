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

/*
 *======================================================================
 * chunk.c: Minimal chunk + FIFO queue utilities for streaming I/O
 *======================================================================
 *
 * Purpose
 * -------
 * Provide a tiny, allocation-efficient payload container (Chunk) and a
 * singly-linked FIFO (ChunkQueue) used by higher layers (e.g., HTTP/3 TX).
 * Payload bytes live inline, immediately after the Chunk header, so a
 * single allocation holds both metadata and data.
 *
 * Data Structures
 * ---------------
 * - Chunk:
 *     * kind     : CH_DATA (payload-bearing node)
 *     * p,len    : current read pointer and remaining bytes
 *     * next     : forward link (singly-linked)
 * - ChunkQueue:
 *     * head/tail: ends of the FIFO
 *     * unread   : total bytes across all nodes
 *     * drained  : accounting of bytes removed/moved by trims/moves
 *
 * Operations (O(1) unless noted)
 * ------------------------------
 * - ChunkAlloc(sz)      : allocate node + inline buffer (uninitialized)
 * - ChunkInit(buf, sz)  : allocate and memcpy() payload
 * - ChunkEnqueue(q, ch) : append node; unread += ch->len
 * - ChunkQueueTrim(q,n) : remove up to n bytes from head (may shrink the
 *                         head by advancing p/len or free whole nodes)
 * - ChunkQueueClear(q)  : drop all nodes (trim SIZE_MAX)
 * - ChunkQueueMove(s,d,m):
 *                         relink whole nodes from src->dst until >= m bytes
 *                         moved (nodes are never split)
 * - ChunkQueuePrint(...) : debug dump of node addresses and lengths
 *
 * Concurrency
 * -----------
 * This module is not thread-safe. Callers must serialize access (e.g.,
 * via a per-stream mutex) when queues are shared.
 *
 * Memory Model
 * ------------
 * Nodes are obtained via ns_malloc(sizeof(Chunk) + payload) and freed by
 * trim/clear paths. Moving between queues relinks nodes (no copies).
 * Trimming advances p/len on the head or frees it entirely.
 *
 * Typical Use (TX path)
 * ---------------------
 *   ch = ChunkInit(buf, len)
 *   ChunkEnqueue(&queued, ch)
 *   // later: move queued->pending, write, then
 *   ChunkQueueTrim(&pending, bytes_written, drain=true)
 *
 * Notes
 * -----
 * - Move may exceed the requested byte budget because nodes aren’t split.
 * - Logging is minimal and intended for debugging.
 */

#include "../include/ns.h"
#include "chunk.h"

/*
 *----------------------------------------------------------------------
 *
 * ChunkAlloc / ChunkInit --
 *
 *      ChunkAlloc creates a CH_DATA chunk with inline payload storage of
 *      size 'sz' in a single allocation: ch->p points just past the
 *      header, ch->len = sz, ch->next = NULL (payload uninitialized).
 *      ChunkInit builds a data chunk by allocating and memcpy'ing
 *      (buffer, sz) into the new chunk.
 *
 * Results:
 *      Both return a Chunk* on success; NULL on allocation failure.
 *
 * Side effects:
 *      Calls ns_malloc(sizeof(Chunk) + sz); ChunkInit also memcpy()s.
 *      No locking/logging. Caller frees with ns_free(). Requires
 *      'buffer' non-NULL when sz > 0.
 *
 *----------------------------------------------------------------------
 */

inline Chunk *
ChunkAlloc(size_t sz)
{
    Chunk *ch = ns_malloc(sizeof(Chunk) + sz);
    if (ch != NULL) {
      ch->kind = CH_DATA;
      ch->p    = (uint8_t *)(ch + 1);
      ch->len  = sz;
      ch->next = NULL;
    }
    return ch;
}

inline Chunk *
ChunkInit(const char *buffer, size_t sz)
{
    Chunk *ch = ChunkAlloc(sz);
    if (ch != NULL) {
      memcpy(ch->p, buffer, sz);
    }
    return ch;
}

/*
 *----------------------------------------------------------------------
 *
 * ChunkEnqueue --
 *
 *      Append 'ch' to the tail of the FIFO queue and bump 'unread'.
 *      Ownership of 'ch' transfers to the queue.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Logs at Notice with 'label'; sets ch->next = NULL; links into
 *      q->head/q->tail and increments q->unread by ch->len. Not
 *      thread-safe; caller must hold the queue's lock if shared.
 *      Memory is later released by ChunkQueueClear/Trim.
 *
 *----------------------------------------------------------------------
 */
inline void
ChunkEnqueue(ChunkQueue *q, Chunk *ch, const char *label)
{
    (void)label;
    //Ns_Log(Notice, "ChunkEnqueue with %ld bytes (%s)", ch->len, label);
    ch->next = NULL;
    if (q->tail != NULL) {
        q->tail->next = ch;
    } else {
        q->head = ch;
    }
    q->tail   = ch;
    q->unread += ch->len;
}

/*
 *----------------------------------------------------------------------
 *
 * ChunkQueueTrim --
 *
 *      Remove up to 'nbytes' from the head of 'q', freeing fully
 *      consumed chunks and shrinking the head chunk when partially
 *      consumed. Preserves FIFO order and updates 'unread'.
 *
 * Results:
 *      Returns the number of bytes actually removed (<= nbytes).
 *
 * Side effects:
 *      Mutates q->head/q->tail and the head chunk's p/len; calls
 *      ns_free() for fully consumed chunks. If 'drain' is true, adds
 *      the requested 'nbytes' to q->drained (not the actual removed).
 *      Not thread-safe; caller must hold the appropriate lock.
 *
 *----------------------------------------------------------------------
 */
size_t
ChunkQueueTrim(ChunkQueue *q, size_t nbytes, bool drain)
{
    size_t remaining = nbytes;

    while (remaining > 0 && q->head) {
        Chunk *ch = q->head;
        if (ch->len <= remaining) {
            /* consume the entire chunk */
            remaining -= ch->len;
            q->unread -= ch->len;
            q->head    = ch->next;
            if (!q->head) {
                q->tail = NULL;
            }
            ns_free(ch);     /* free the chunk node */
        } else {
            /* only part of this chunk */
            ch->p   += remaining;
            ch->len -= remaining;
            q->unread -= remaining;
            remaining = 0;
        }
    }
    if (drain) {
        q->drained += nbytes;
    }
    return nbytes - remaining;
}

/*
 *----------------------------------------------------------------------
 *
 * ChunkQueueClear --
 *
 *      Empty 'q' by trimming SIZE_MAX with drain=false.
 *
 * Results:
 *      Bytes removed.
 *
 * Side effects:
 *      Frees all chunks; sets head/tail = NULL and unread = 0. Not
 *      thread-safe; caller must synchronize.
 *
 *----------------------------------------------------------------------
 */
size_t
ChunkQueueClear(ChunkQueue *q) {
  return ChunkQueueTrim(q, SIZE_MAX, NS_FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * ChunkQueueMove --
 *
 *      Move data from 'src' to 'dst' by relinking whole chunks in FIFO
 *      order until 'maxbytes' is reached. Chunks are not split, so the
 *      total 'moved' may exceed 'maxbytes'. Ownership transfers to 'dst'.
 *
 * Results:
 *      Returns total bytes moved (>= 0).
 *
 * Side effects:
 *      Updates head/tail and unread of both queues; increments
 *      src->drained by 'moved'. No allocation or copies. Not
 *      thread-safe-c aller must synchronize.
 *
 *----------------------------------------------------------------------
 */
size_t
ChunkQueueMove(ChunkQueue *src, ChunkQueue *dst, size_t maxbytes)
{
    size_t moved = 0;

    while (src->head && moved < maxbytes) {
        Chunk *ch = src->head;
        size_t   take = ch->len; /* take whole node */

        /* detach from src */
        src->head = ch->next;
        if (!src->head) src->tail = NULL;
        src->unread -= take;

        /* append to dst */
        ch->next = NULL;
        if (dst->tail) dst->tail->next = ch; else dst->head = ch;
        dst->tail   = ch;
        dst->unread += take;

        moved += take;
    }
    src->drained += moved;
    return moved;
}

/*
 *----------------------------------------------------------------------
 *
 * ChunkQueuePrint --
 *
 *      Debug helper: dump queue contents to logs, prefixed by 'msg'.
 *      Walks from head to tail and prints each chunk’s address and len.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Emits Notice logs; no mutation or allocation. Not thread-safe,
 *      caller must synchronize if the queue is shared.
 *
 *----------------------------------------------------------------------
 */
void
ChunkQueuePrint(const char *msg, ChunkQueue *q)
{
    Chunk *ch = q->head;

    Ns_Log(Notice, "H3 ChunkQueuePrint %s starting with %p", msg, (void*)ch);
    while (ch != NULL) {
        Ns_Log(Notice, "H3 ... chunks len %ld %p", ch->len, (void*) ch);
        ch = ch->next;
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
