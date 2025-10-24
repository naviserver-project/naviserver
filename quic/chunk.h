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
#ifndef H3_CHUNK_H
# define H3_CHUNK_H

# ifdef __cplusplus
extern "C" {
# endif

typedef enum { CH_DATA = 0 } chunk_kind_t;

typedef struct Chunk {
    chunk_kind_t   kind;
    uint8_t       *p;        /* current read ptr */
    size_t         len;      /* unread bytes left */
    struct Chunk   *next;
    /* data[] follows */
} Chunk;

typedef struct ChunkQueue {
    Chunk  *head, *tail;
    size_t    unread;
    size_t    drained;   // just for debugging
} ChunkQueue;


Chunk *
ChunkAlloc(size_t sz);

Chunk *
ChunkInit(const char *buffer, size_t sz)
  NS_GNUC_NONNULL(1);

void
ChunkEnqueue(ChunkQueue *q, Chunk *ch, const char *label)
 NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

size_t
ChunkQueueTrim(ChunkQueue *q, size_t nbytes, bool drain)
  NS_GNUC_NONNULL(1);

size_t
ChunkQueueClear(ChunkQueue *q)
  NS_GNUC_NONNULL(1);

size_t
ChunkQueueMove(ChunkQueue *src, ChunkQueue *dst,size_t maxbytes)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

void ChunkQueuePrint(const char *msg, ChunkQueue *q)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_UNUSED;

# ifdef __cplusplus
}
# endif

#endif /* H3_CHUNK_H */
