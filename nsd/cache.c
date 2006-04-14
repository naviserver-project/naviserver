/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 * 
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */


/* 
 * cache.c --
 *
 *      Size and time limited caches.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

struct Cache;

/*
 * An Entry is a node in a linked list as well as being a
 * hash table entry. The linked list is there to keep track of
 * usage for the purposes of cache pruning.
 */

typedef struct Entry {
    struct Entry   *nextPtr;
    struct Entry   *prevPtr;
    struct Cache   *cachePtr;
    Tcl_HashEntry  *hPtr;
    Ns_Time         expires;
    size_t          size;
    void           *value;
} Entry;

/*
 * The following structure defines a cache
 */

typedef struct Cache {
    Entry         *firstEntryPtr;
    Entry         *lastEntryPtr;
    Tcl_HashEntry *hPtr;
    int            keys;
    time_t         ttl;
    size_t         maxSize;
    size_t         currentSize;
    Ns_Callback   *freeProc;
    Ns_Mutex       lock;
    Ns_Cond        cond;
    unsigned int   nhit;
    unsigned int   nmiss;
    unsigned int   nflushed;
    unsigned int   npruned;
    unsigned int   nexpired;
    Tcl_HashTable  entriesTable;
} Cache;


/*
 * Local functions defined in this file
 */

static int  Expired(Entry *ePtr);
static void Delink(Entry *ePtr);
static void Push(Entry *ePtr);
static void ExpireEntry(Entry *ePtr);
static void PruneEntry(Entry *ePtr);


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheCreate, Ns_CacheCreateSz, Ns_CacheCreateEx --
 *
 *      Create a new time and/or size based cache.
 *
 * Results:
 *      A pointer to the new cache.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Cache *
Ns_CacheCreate(CONST char *name, int keys, time_t ttl, Ns_Callback *freeProc)
{
    return Ns_CacheCreateEx(name, keys, ttl, 0, freeProc);
}

Ns_Cache *
Ns_CacheCreateSz(CONST char *name, int keys, size_t maxSize, Ns_Callback *freeProc)
{
    return Ns_CacheCreateEx(name, keys, -1, maxSize, freeProc);
}

Ns_Cache *
Ns_CacheCreateEx(CONST char *name, int keys, time_t ttl, size_t maxSize,
                 Ns_Callback *freeProc)
{
    Cache *cachePtr;

    cachePtr = ns_calloc(1, sizeof(Cache));
    cachePtr->freeProc       = freeProc;
    cachePtr->ttl            = ttl;
    cachePtr->maxSize        = maxSize;
    cachePtr->currentSize    = 0;
    cachePtr->keys           = keys;
    cachePtr->nhit           = 0;
    cachePtr->nmiss          = 0;
    cachePtr->nflushed       = 0;
    cachePtr->npruned        = 0;
    cachePtr->nexpired       = 0;

    Ns_MutexSetName2(&cachePtr->lock, "ns:cache", name);
    Tcl_InitHashTable(&cachePtr->entriesTable, keys);

    return (Ns_Cache *) cachePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheDestroy
 *
 *      Flush all entries and delete a cache.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Cache no longer usable.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheDestroy(Ns_Cache *cache)
{
    Cache *cachePtr = (Cache *) cache;

    Ns_CacheFlush(cache);
    Ns_MutexDestroy(&cachePtr->lock);
    Ns_CondDestroy(&cachePtr->cond);
    Tcl_DeleteHashTable(&cachePtr->entriesTable);
    ns_free(cachePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheFindEntry --
 *
 *      Find a cache entry given it's key.
 *
 * Results:
 *      A pointer to an Ns_Entry cache entry, or NULL if the key does
 *      not exist or the entry has expired.
 *
 * Side effects:
 *      The cache entry will be flushed if it has expired, or will
 *      move to the top of the LRU list if valid.
 *
 *----------------------------------------------------------------------
 */

Ns_Entry *
Ns_CacheFindEntry(Ns_Cache *cache, CONST char *key)
{
    Cache         *cachePtr = (Cache *) cache;
    Tcl_HashEntry *hPtr;
    Entry         *ePtr;

    hPtr = Tcl_FindHashEntry(&cachePtr->entriesTable, key);
    if (hPtr == NULL) {
        ++cachePtr->nmiss;
        return NULL;
    }
    ePtr = Tcl_GetHashValue(hPtr);
    if (Expired(ePtr)) {
        ExpireEntry(ePtr);
        ++cachePtr->nmiss;
        return NULL;
    };
    ++cachePtr->nhit;
    Delink(ePtr);
    Push(ePtr);

    return (Ns_Entry *) ePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheCreateEntry, Ns_CacheWaitCreateEntry --
 *
 *      Create a new cache entry or return an existing one with the
 *      given key.  Wait up to timeout seconds for another thread to
 *      complete an update.
 *
 * Results:
 *      A pointer to a new cache entry or NULL on timeout or flush.
 *
 * Side effects:
 *      Memory will be allocated for the new cache entry and it will 
 *      be inserted into the cache.
 *
 *      Cache lock may be released and re-acquired.
 *
 *----------------------------------------------------------------------
 */

Ns_Entry *
Ns_CacheCreateEntry(Ns_Cache *cache, CONST char *key, int *newPtr)
{
    return Ns_CacheWaitCreateEntry(cache, key, newPtr, 0);
}

Ns_Entry *
Ns_CacheWaitCreateEntry(Ns_Cache *cache, CONST char *key, int *newPtr, time_t timeout)
{
    Cache         *cachePtr = (Cache *) cache;
    Tcl_HashEntry *hPtr;
    Entry         *ePtr;
    Ns_Time        time;
    CONST char    *value;
    int            status;

    hPtr = Tcl_CreateHashEntry(&cachePtr->entriesTable, key, newPtr);
    if (*newPtr) {
        ++cachePtr->nmiss;
        ePtr = ns_calloc(1, sizeof(Entry));
        ePtr->hPtr = hPtr;
        ePtr->cachePtr = cachePtr;
        Tcl_SetHashValue(hPtr, ePtr);
        Push(ePtr);
    } else {
        ePtr = Tcl_GetHashValue(hPtr);

        if (timeout > 0 && (value = Ns_CacheGetValue((Ns_Entry *) ePtr)) == NULL) {

            /*
             * Wait for another thread to complete an update.
             */

            Ns_GetTime(&time);
            Ns_IncrTime(&time, timeout, 0);

            status = NS_OK;
            do {
                status = Ns_CacheTimedWait(cache, &time);
            } while (status == NS_OK
                     && (ePtr = (Entry *) Ns_CacheFindEntry(cache, key)) != NULL
                     && (value = Ns_CacheGetValue((Ns_Entry *) ePtr)) == NULL);
            if (ePtr == NULL || value == NULL) {
                return NULL;
            }
        }

        /*
         * The entry may have expired already when we come here.
         * We have two choices:
         *    o. re-use the expired entry (and save ourselves some time)
         *    o. delete the entry and re-create it again
         * Opt to first choice for now, until somebody complains.
         */

        ++cachePtr->nhit;
    }

    return (Ns_Entry *) ePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheKey --
 *
 *      Gets the key of a cache entry.
 *
 * Results:
 *      A pointer to the key for the given entry.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_CacheKey(Ns_Entry *entry)
{
    Entry *ePtr = (Entry *) entry;

    return Tcl_GetHashKey(&ePtr->cachePtr->entriesTable, ePtr->hPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetValue --
 *
 *      Get the value (contents) of a cache entry.
 *
 * Results:
 *      A pointer to the cache entry's contents.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_CacheGetValue(Ns_Entry *entry)
{
    Entry *ePtr = (Entry *) entry;

    return ePtr->value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetSize --
 *
 *      Get the size of the value (contents) of a cache entry.
 *
 * Results:
 *      The size in bytes or 0 if the size is unknown.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_CacheGetSize(Ns_Entry *entry)
{
    Entry *ePtr = (Entry *) entry;

    return ePtr->size;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheSetValue, Ns_CacheSetValueSz --
 *
 *      Free the cache entry's previous contents, set it to the new 
 *      contents, increase the size of the cache, and prune until 
 *      it's back under the maximum size. 
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Cache pruning and freeing of old contents may occur. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheSetValue(Ns_Entry *entry, void *value)
{
    Ns_CacheSetValueSz(entry, value, 0);
}

void
Ns_CacheSetValueSz(Ns_Entry *entry, void *value, size_t size)
{
    Ns_CacheSetValueExpires(entry, value, size, 0);
}

void
Ns_CacheSetValueExpires(Ns_Entry *entry, void *value, size_t size, time_t ttl)
{
    Entry   *ePtr = (Entry *) entry;
    Cache   *cachePtr = ePtr->cachePtr;
    Ns_Time  now;

    Ns_CacheUnsetValue(entry);
    ePtr->value = value;
    ePtr->size = size;
    if (ttl > 0) {
        Ns_GetTime(&now);
        Ns_IncrTime(&ePtr->expires, ttl, 0);
    }
    cachePtr->currentSize += size;
    if (ePtr->cachePtr->maxSize > 0) {
        while (cachePtr->currentSize > cachePtr->maxSize &&
               cachePtr->lastEntryPtr != ePtr) {
            PruneEntry(cachePtr->lastEntryPtr);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheUnsetValue --
 *
 *      Reset the value of an entry to NULL, calling the free proc for
 *      any previous entry and updating the cache size.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheUnsetValue(Ns_Entry *entry)
{
    Entry *ePtr = (Entry *) entry;
    Cache *cachePtr;
 
    if (ePtr->value != NULL) {
        cachePtr = ePtr->cachePtr;
        cachePtr->currentSize -= ePtr->size;
        if (cachePtr->freeProc != NULL) {
            (*cachePtr->freeProc)(ePtr->value);
        }
        ePtr->size = 0;
        ePtr->value = NULL;
        ePtr->expires.sec = ePtr->expires.usec = 0;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheDeleteEntry --
 *
 *      Delete an entry from the cache table and free memory.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheDeleteEntry(Ns_Entry *entry)
{
    Entry *ePtr = (Entry *) entry;

    Delink(ePtr);
    Tcl_DeleteHashEntry(ePtr->hPtr);
    ns_free(ePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheFlushEntry --
 *
 *      Delete an entry from the cache table after first unsetting
 *      the current entry value (if any).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Statistics updated.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheFlushEntry(Ns_Entry *entry)
{
    Entry *ePtr = (Entry *) entry;

    ++ePtr->cachePtr->nflushed;
    Ns_CacheUnsetValue(entry);
    Ns_CacheDeleteEntry(entry);

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheFlush --
 *
 *      Flush every entry from a cache.
 *
 * Results:
 *      Number of entries flushed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_CacheFlush(Ns_Cache *cache)
{
    Ns_CacheSearch  search;
    Ns_Entry       *entry;
    int             nflushed = 0;

    entry = Ns_CacheFirstEntry(cache, &search);
    while (entry != NULL) {
        Ns_CacheFlushEntry(entry);
        entry = Ns_CacheNextEntry(&search);
        nflushed++;
    }
    return nflushed;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheFirstEntry --
 *
 *      Return a pointer to the first entry in the cache (in no
 *      particular order).
 *
 * Results:
 *      A pointer to said entry.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Entry *
Ns_CacheFirstEntry(Ns_Cache *cache, Ns_CacheSearch *search)
{
    Cache          *cachePtr = (Cache *) cache;
    Tcl_HashSearch *sPtr = (Tcl_HashSearch *) search;
    Tcl_HashEntry  *hPtr;

    hPtr = Tcl_FirstHashEntry(&cachePtr->entriesTable, sPtr);
    if (hPtr == NULL) {
        return NULL;
    }
    return (Ns_Entry *) Tcl_GetHashValue(hPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheNextEntry --
 *
 *      When used in conjunction with Ns_CacheFirstEntry, one may
 *      walk through the whole cache.
 *
 * Results:
 *      Pointer to next entry, or NULL when all entries visited.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Entry *
Ns_CacheNextEntry(Ns_CacheSearch *search)
{
    Tcl_HashSearch *sPtr = (Tcl_HashSearch *) search;
    Tcl_HashEntry  *hPtr;

    hPtr = Tcl_NextHashEntry(sPtr);
    if (hPtr == NULL) {
        return NULL;
    }
    return (Ns_Entry *) Tcl_GetHashValue(hPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheLock --
 *
 *      Lock the cache.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Mutex locked.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheLock(Ns_Cache *cache)
{
    Cache *cachePtr = (Cache *) cache;

    Ns_MutexLock(&cachePtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheTryLock --
 *
 *      Try to lock the cache.
 *
 * Results:
 *      NS_OK if successfully locked, NS_TIMEOUT if already locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_CacheTryLock(Ns_Cache *cache)
{
    Cache *cachePtr = (Cache *) cache;

    return Ns_MutexTryLock(&cachePtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheUnlock --
 *
 *      Unlock the cache.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheUnlock(Ns_Cache *cache)
{
    Cache *cachePtr = (Cache *) cache;

    Ns_MutexUnlock(&cachePtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheWait, Ns_CacheTimedWait --
 *
 *      Wait for the cache's condition variable to be signaled or for
 *      the given absolute timeout if timePtr is not NULL.
 *
 * Results:
 *      NS_OK or NS_TIMEOUT if timeout specified.
 *
 * Side effects:
 *      Thread is suspended until condition is signaled or timeout.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheWait(Ns_Cache *cache)
{
    Ns_CacheTimedWait(cache, NULL);
}

int
Ns_CacheTimedWait(Ns_Cache *cache, Ns_Time *timePtr)
{
    Cache *cachePtr = (Cache *) cache;
    
    return Ns_CondTimedWait(&cachePtr->cond, &cachePtr->lock, timePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheSignal --
 *
 *      Signal the cache's condition variable, waking the first waiting
 *      thread (if any).
 *
 *      NOTE:  Be sure you don't really want to wake all threads with
 *      Ns_CacheBroadcast.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A single thread may resume.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheSignal(Ns_Cache *cache)
{
    Cache *cachePtr = (Cache *) cache;
    
    Ns_CondSignal(&cachePtr->cond);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheBroadcast --
 *
 *      Broadcast the cache's condition variable, waking all waiting
 *      threads (if any).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Waiting threads may resume.
 *
 *----------------------------------------------------------------------
 */
    
void
Ns_CacheBroadcast(Ns_Cache *cache)
{
    Cache *cachePtr = (Cache *) cache;
    
    Ns_CondBroadcast(&cachePtr->cond);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheStats --
 *
 *      Append statistics about cache usage to dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheStats(Ns_Cache *cache, Ns_DString *dest)
{
    Cache        *cachePtr = (Cache *) cache;
    unsigned int  total, hitrate;

    total = cachePtr->nhit + cachePtr->nmiss;
    hitrate = (total ? (cachePtr->nhit * 100) / total : 0);

    Ns_DStringPrintf(dest, "maxsize %lu size %lu entries %d "
                     "flushed %u hits %u missed %u hitrate %u "
                     "expired %u pruned %u",
                     (unsigned long) cachePtr->maxSize,
                     (unsigned long) cachePtr->currentSize,
                     cachePtr->entriesTable.numEntries, cachePtr->nflushed,
                     cachePtr->nhit, cachePtr->nmiss, hitrate,
                     cachePtr->nexpired, cachePtr->npruned);
}


/*
 *----------------------------------------------------------------------
 *
 * Expired --
 *
 *      Check if an entry has expired.
 *
 * Results:
 *      1 if entry has expired, 0 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Expired(Entry *ePtr)
{
    Ns_Time  now;

    if (ePtr->expires.sec > 0 || ePtr->expires.usec > 0) {
        Ns_GetTime(&now);
        if (Ns_DiffTime(&ePtr->expires, &now, NULL) < 0) {
            return 1;
        }
    }
    return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Delink --
 *
 *      Remove a cache entry from the linked list of entries; this
 *      is used for maintaining the LRU list as well as removing entries
 *      that are still in use.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
 
static void
Delink(Entry *ePtr)
{
    if (ePtr->prevPtr != NULL) {
        ePtr->prevPtr->nextPtr = ePtr->nextPtr;
    } else {
        ePtr->cachePtr->firstEntryPtr = ePtr->nextPtr;
    }
    if (ePtr->nextPtr != NULL) {
        ePtr->nextPtr->prevPtr = ePtr->prevPtr;
    } else {
        ePtr->cachePtr->lastEntryPtr = ePtr->prevPtr;
    }
    ePtr->prevPtr = ePtr->nextPtr = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Push --
 *
 *      Push an entry to the top of the linked list of entries, making
 *      it the Most Recently Used
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
Push(Entry *ePtr)
{
    if (ePtr->cachePtr->firstEntryPtr != NULL) {
        ePtr->cachePtr->firstEntryPtr->prevPtr = ePtr;
    }
    ePtr->prevPtr = NULL;
    ePtr->nextPtr = ePtr->cachePtr->firstEntryPtr;
    ePtr->cachePtr->firstEntryPtr = ePtr;
    if (ePtr->cachePtr->lastEntryPtr == NULL) {
        ePtr->cachePtr->lastEntryPtr = ePtr;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ExpireEntry --
 *
 *      Flush an expired entry.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
ExpireEntry(Entry *ePtr)
{
    ++ePtr->cachePtr->nexpired;

    Ns_CacheUnsetValue((Ns_Entry *)ePtr);
    Ns_CacheDeleteEntry((Ns_Entry *)ePtr);

    return;
}

/*
 *----------------------------------------------------------------------
 *
 * PruneEntry --
 *
 *      Flush an entry to adjust the size of the cache.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
PruneEntry(Entry *ePtr)
{
    ++ePtr->cachePtr->npruned;

    Ns_CacheUnsetValue((Ns_Entry *)ePtr);
    Ns_CacheDeleteEntry((Ns_Entry *)ePtr);

    return;
}
