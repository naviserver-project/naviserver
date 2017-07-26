/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
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
    Ns_Time         expires;          /* Absolute ttl timeout. */
    size_t          size;
    int		    cost;             /* cost to compute a single entry */
    int		    count;            /* reuse count of this entry */
    void           *value;            /* Will appear NULL for concurrent updates. */
    void           *uncommittedValue; /* Used for transactional mode */
    uintptr_t       transactionEpoch; /* Used for identifying transaction */
} Entry;

/*
 * The following structure defines a cache
 */

typedef struct Cache {
    Entry         *firstEntryPtr;
    Entry         *lastEntryPtr;
    int            keys;
    size_t         maxSize;
    size_t         currentSize;
    Ns_Callback   *freeProc;
    Ns_Mutex       lock;
    Ns_Cond        cond;
    Tcl_HashTable  entriesTable;
    uintptr_t      transactionEpoch;
    Tcl_HashTable  uncommittedTable;
    struct {
        unsigned long   nhit;      /* Successful gets. */
        unsigned long   nmiss;     /* Unsuccessful gets. */
        unsigned long   nexpired;  /* Unsuccessful gets due to entry expiry. */
        unsigned long   nflushed;  /* Explicit flushes by user code. */
        unsigned long   npruned;   /* Evictions due to size constraint. */
        unsigned long   ncommit;   /* number of commits. */
        unsigned long   nrollback; /* number of rollback operations. */
    } stats;

    char name[1];

} Cache;


/*
 * Local functions defined in this file
 */

static bool Expired(const Entry *ePtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1);

static void Delink(Entry *ePtr)
    NS_GNUC_NONNULL(1);

static void Push(Entry *ePtr)
    NS_GNUC_NONNULL(1);

static unsigned long
CacheTransaction(Cache *cachePtr, uintptr_t epoch, bool commit)
    NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheCreateSz --
 *
 *      Create a new size limited cache.
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
Ns_CacheCreateSz(const char *name, int keys, size_t maxSize, Ns_Callback *freeProc)
{
    Cache *cachePtr;
    size_t nameLength;

    NS_NONNULL_ASSERT(name != NULL);

    nameLength = strlen(name);

    cachePtr = ns_calloc(1u, sizeof(Cache) + nameLength);
    memcpy(cachePtr->name, name, nameLength + 1u);
    
    cachePtr->freeProc        = freeProc;
    cachePtr->maxSize         = maxSize;
    cachePtr->currentSize     = 0u;
    cachePtr->keys            = keys;
    cachePtr->stats.nhit      = 0u;
    cachePtr->stats.nmiss     = 0u;
    cachePtr->stats.nexpired  = 0u;
    cachePtr->stats.nflushed  = 0u;
    cachePtr->stats.npruned   = 0u;
    cachePtr->stats.ncommit   = 0u;
    cachePtr->stats.nrollback = 0u;

    Ns_MutexInit(&cachePtr->lock);
    Ns_MutexSetName2(&cachePtr->lock, "ns:cache", name);
    Tcl_InitHashTable(&cachePtr->entriesTable, keys);
    Tcl_InitHashTable(&cachePtr->uncommittedTable, TCL_ONE_WORD_KEYS);

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
 *      Statistics logged, cache no longer usable.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheDestroy(Ns_Cache *cache)
{
    Cache      *cachePtr = (Cache *) cache;

    NS_NONNULL_ASSERT(cache != NULL);
    
    (void) Ns_CacheFlush(cache);
    Ns_MutexDestroy(&cachePtr->lock);
    Ns_CondDestroy(&cachePtr->cond);
    Tcl_DeleteHashTable(&cachePtr->entriesTable);
    Tcl_DeleteHashTable(&cachePtr->uncommittedTable);
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
 *      A valid entry will move to the top of the LRU list.
 *
 *----------------------------------------------------------------------
 */
Ns_Entry *
Ns_CacheFindEntry(Ns_Cache *cache, const char *key)
{
    return Ns_CacheFindEntryT(cache, key, NULL);
}

Ns_Entry *
Ns_CacheFindEntryT(Ns_Cache *cache, const char *key, const Ns_CacheTransactionStack *transactionStackPtr)
{
    Cache               *cachePtr = (Cache *) cache;
    const Tcl_HashEntry *hPtr;
    Ns_Entry            *result;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    hPtr = Tcl_FindHashEntry(&cachePtr->entriesTable, key);
    if (unlikely(hPtr == NULL)) {
        /*
         * Entry does not exist at all.
         */
        ++cachePtr->stats.nmiss;
        result = NULL;

    } else {
        Entry *ePtr = Tcl_GetHashValue(hPtr);

        if (unlikely(ePtr->value == NULL && transactionStackPtr->depth == 0)) {
            /*
             * Entry is being updated by some other thread.
             */
            ++cachePtr->stats.nmiss;
            result = NULL;

        } else if (unlikely(Expired(ePtr, NULL))) {
            /*
             * Entry exists but has expired.
             */
            Ns_CacheDeleteEntry((Ns_Entry *) ePtr);
            ++cachePtr->stats.nmiss;
            result = NULL;

        } else {
            void *value;

            if (ePtr->value == NULL) {
                value = Ns_CacheGetValueT((Ns_Entry *) ePtr, transactionStackPtr);
                if (value == NULL) {
                    ++cachePtr->stats.nmiss;
                }
            } else {
                value = ePtr->value;
            }
            if (value != NULL) {
                /*
                 * Entry is valid.
                 */
                ++cachePtr->stats.nhit;
                Delink(ePtr);
                ePtr->count ++;
                Push(ePtr);
                result = (Ns_Entry *) ePtr;
            } else {
                result = NULL;
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheCreateEntry --
 *
 *      Create a new cache entry or return an existing one with the
 *      given key.
 *
 * Results:
 *      A pointer to a cache entry.
 *
 * Side effects:
 *      Existing, expired entries will be unset and reported as new.
 *
 *----------------------------------------------------------------------
 */

Ns_Entry *
Ns_CacheCreateEntry(Ns_Cache *cache, const char *key, int *newPtr)
{
    Cache         *cachePtr = (Cache *) cache;
    Tcl_HashEntry *hPtr;
    Entry         *ePtr;
    int            isNew;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(newPtr != NULL);

    hPtr = Tcl_CreateHashEntry(&cachePtr->entriesTable, key, &isNew);
    if (isNew != 0) {
        ePtr = ns_calloc(1u, sizeof(Entry));
        ePtr->hPtr = hPtr;
        ePtr->cachePtr = cachePtr;
        Tcl_SetHashValue(hPtr, ePtr);
        ++cachePtr->stats.nmiss;
    } else {
        ePtr = Tcl_GetHashValue(hPtr);
        if (Expired(ePtr, NULL)) {
            ++cachePtr->stats.nexpired;
            Ns_CacheUnsetValue((Ns_Entry *) ePtr);
            isNew = 1;
        } else {
	    ePtr->count ++;
            ++cachePtr->stats.nhit;
        }
        Delink(ePtr);
    }
    Push(ePtr);
    *newPtr = isNew;

    return (Ns_Entry *) ePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheWaitCreateEntry --
 *
 *      Create a new cache entry or return an existing one with the
 *      given key.  Wait until the given absolute timeout for another
 *      thread to complete an update.
 *
 * Results:
 *      A pointer to a new cache entry, or NULL on timeout.
 *
 * Side effects:
 *      Cache lock may be released and re-acquired.
 *
 *----------------------------------------------------------------------
 */
Ns_Entry *
Ns_CacheWaitCreateEntry(Ns_Cache *cache, const char *key, int *newPtr,
                        const Ns_Time *timeoutPtr)
{
    return Ns_CacheWaitCreateEntryT(cache, key, newPtr, timeoutPtr,  NULL);
}

Ns_Entry *
Ns_CacheWaitCreateEntryT(Ns_Cache *cache, const char *key, int *newPtr,
                        const Ns_Time *timeoutPtr, const Ns_CacheTransactionStack *transactionStackPtr)
{
    Ns_Entry      *entry;
    int            isNew;
    Ns_ReturnCode  status = NS_OK;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(newPtr != NULL);

    entry = Ns_CacheCreateEntry(cache, key, &isNew);
    if (isNew == 0 && Ns_CacheGetValueT(entry, transactionStackPtr) == NULL) {
        do {
            status = Ns_CacheTimedWait(cache, timeoutPtr);
            entry = Ns_CacheCreateEntry(cache, key, &isNew);
        } while (status == NS_OK
                 && isNew == 0
                 && Ns_CacheGetValueT(entry, transactionStackPtr) == NULL);
    }
    *newPtr = isNew;

    return status == NS_OK ? entry : NULL;
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

const char *
Ns_CacheKey(const Ns_Entry *entry)
{
    const Entry *ePtr;

    NS_NONNULL_ASSERT(entry != NULL);

    ePtr = (Entry *) entry;
    return Tcl_GetHashKey(&ePtr->cachePtr->entriesTable, ePtr->hPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetValue, Ns_CacheGetSize, Ns_CacheGetExpirey,
 * Ns_CacheGetTransactionEpoch --
 *
 *      Get the bare components of a cache entry via API.
 *
 * Results:
 *      As specified.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_CacheGetValue(const Ns_Entry *entry)
{
    NS_NONNULL_ASSERT(entry != NULL);
    return ((const Entry *) entry)->value;
}

size_t
Ns_CacheGetSize(const Ns_Entry *entry)
{
    NS_NONNULL_ASSERT(entry != NULL);
    return ((const Entry *) entry)->size;
}

const Ns_Time *
Ns_CacheGetExpirey(const Ns_Entry *entry)
{
    NS_NONNULL_ASSERT(entry != NULL);
    return &((const Entry *) entry)->expires;
}

uintptr_t
Ns_CacheGetTransactionEpoch(const Ns_Entry *entry)
{
    NS_NONNULL_ASSERT(entry != NULL);
    return ((const Entry *) entry)->transactionEpoch;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetValueT --
 *
 *      Get the value a cache entry, respecting the cache transaction
 *      stack. The functio returns either the bare stack value (if present) or
 *      the uncommitted value, when called from within a cache transaction.
 *
 * Results:
 *      The cache value or NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
Ns_CacheGetValueT(const Ns_Entry *entry, const Ns_CacheTransactionStack *transactionStackPtr)
{
    const Entry *e;
    void        *result;

    NS_NONNULL_ASSERT(entry != NULL);

    e = (const Entry *)entry;
    /*
     * In case, there is a value, we are sure this is a committed value.
     */
    if (e->value != NULL) {
        result = e->value;
    } else {
        /*
         * If there is no value, it might be an ongoing ns_cache_eval in
         * another thread or there might be an uncommitted value from the same
         * or another thread. In these cases this function might return NULL.
         */

        result = NULL;
        if (transactionStackPtr != NULL) {
            unsigned int i;

            for (i = 0; i < transactionStackPtr->depth; i++) {
                if (e->transactionEpoch == transactionStackPtr->stack[i]) {
                    result = e->uncommittedValue;
                    break;
                }
            }
        }
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetNrUncommitedEntries --
 *
 *      Return for a given cache the number of uncommitted entries.
 *
 * Results:
 *      positive integer, type align with Tcl Hash Tables
 *
 * Side effects:
 *      Nonoe.
 *
 *----------------------------------------------------------------------
 */
int
Ns_CacheGetNrUncommitedEntries(const Ns_Cache *cache)
{
    const Cache *cachePtr;

    NS_NONNULL_ASSERT(cache != NULL);

    cachePtr = (const Cache *)cache;
    return cachePtr->uncommittedTable.numEntries;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheSetValue, Ns_CacheSetValueSz, Ns_CacheSetValueExpires --
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
    NS_NONNULL_ASSERT(entry != NULL);
    NS_NONNULL_ASSERT(value != NULL);
    Ns_CacheSetValueSz(entry, value, 0u);
}

void
Ns_CacheSetValueSz(Ns_Entry *entry, void *value, size_t size)
{
    NS_NONNULL_ASSERT(entry != NULL);
    NS_NONNULL_ASSERT(value != NULL);
    (void)Ns_CacheSetValueExpires(entry, value, size, NULL, 0, 0u, 0u);
}

int
Ns_CacheSetValueExpires(Ns_Entry *entry, void *value, size_t size,
                        const Ns_Time *timeoutPtr, int cost, size_t maxSize,
                        uintptr_t transactionEpoch)
{
    Entry *ePtr;
    Cache *cachePtr;
    int    result;

    NS_NONNULL_ASSERT(entry != NULL);

    ePtr = (Entry *) entry;
    cachePtr = ePtr->cachePtr;

    Ns_CacheUnsetValue(entry);

    if (transactionEpoch == 0) {
        ePtr->value = value;
        result = 0;
    } else {
        Tcl_HashEntry *hPtr;
        int            isNew;

        ePtr->uncommittedValue = value;
        ePtr->transactionEpoch = transactionEpoch;

        hPtr = Tcl_CreateHashEntry(&cachePtr->uncommittedTable, ePtr, &isNew);

        fprintf(stderr, "... add entry %p (cache %s value %s) to pending table (pending %d)\n",
                (void*)ePtr, cachePtr->name, value, cachePtr->uncommittedTable.numEntries);

        if (unlikely(isNew == 0)) {
            Ns_Log(Warning, "cache %s: adding entry %p with value '%s' multiple times to pending table",
                   ePtr->cachePtr->name, (void *)ePtr, value);
        }

        result = 1;
    }
    ePtr->size = size;
    ePtr->cost = cost;
    ePtr->count = 1;

    if (timeoutPtr != NULL) {
        ePtr->expires = *timeoutPtr;
    }
    cachePtr->currentSize += size;

    if (maxSize == 0u) {
        /*
         * Use the maxSize setting as configured in cPtr
         */
        maxSize = cachePtr->maxSize;
    } else if (maxSize != cachePtr->maxSize) {
        /*
         * Update the cache max size via the provided setting
         */
        cachePtr->maxSize = maxSize;
    }

    if (maxSize > 0u) {
        /*
	 * Make space for the new entry, but don't delete the current
	 * entry, and don't delete other newborn entries (with a value
	 * of NULL) of some other threads which are concurrently
	 * created.  There might be concurrent updates, since
	 * e.g. nscache_eval releases its mutex.
	 */
        while (cachePtr->currentSize > maxSize &&
               cachePtr->lastEntryPtr != ePtr &&
               cachePtr->lastEntryPtr->value != NULL) {
            Ns_CacheDeleteEntry((Ns_Entry *) cachePtr->lastEntryPtr);
            ++cachePtr->stats.npruned;
        }
    }
    return result;
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
    Entry *ePtr;
    void *value;

    NS_NONNULL_ASSERT(entry != NULL);

    ePtr = (Entry *) entry;
    value = ePtr->value;

    if (value != NULL) {
        Cache *cachePtr;

	/*
	 * In case, the freeProc() wants to allocate itself
	 * (indirectly) a cache entry, we have to make sure, that
	 * ePtr->value is not freed twice. Therefore, we keep the
	 * affected member "value" in a local variable and set
	 * ePtr->value to NULL before it is actually deallocated and
	 * call the freeProc after updating all entry members.
	 */
        cachePtr = ePtr->cachePtr;
        cachePtr->currentSize -= ePtr->size;
	ePtr->size = 0u;
        ePtr->value = NULL;
        ePtr->expires.sec = ePtr->expires.usec = 0;
		
        if (cachePtr->freeProc != NULL) {
            (*cachePtr->freeProc)(value);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheFlushEntry, Ns_CacheDeleteEntry --
 *
 *      Delete an entry from the cache table after first unsetting
 *      the current entry value (if any).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Statistics updated on flush.
 *      NB: Entries under concurrent update are also deleted.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheFlushEntry(Ns_Entry *entry)
{
    const Entry *ePtr;

    NS_NONNULL_ASSERT(entry != NULL);

    ePtr = (Entry *) entry;
    ePtr->cachePtr->stats.nflushed++;
    Ns_CacheDeleteEntry(entry);
}

void
Ns_CacheDeleteEntry(Ns_Entry *entry)
{
    Entry         *ePtr;
    Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(entry != NULL);

    ePtr = (Entry *) entry;
    Ns_CacheUnsetValue(entry);
    Delink(ePtr);
    Tcl_DeleteHashEntry(ePtr->hPtr);

    hPtr = Tcl_FindHashEntry(&ePtr->cachePtr->uncommittedTable, ePtr);
    if (unlikely(hPtr != NULL)) {
        Tcl_DeleteHashEntry(hPtr);
    }

    ns_free(ePtr);
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
 *      Cache stats updated, concurrent updates skipped.
 *
 *----------------------------------------------------------------------
 */

int
Ns_CacheFlush(Ns_Cache *cache)
{
    Cache          *cachePtr;
    Ns_CacheSearch  search;
    Ns_Entry       *entry;
    int             nflushed = 0;

    NS_NONNULL_ASSERT(cache != NULL);

    cachePtr = (Cache *) cache;
    entry = Ns_CacheFirstEntry(cache, &search);
    while (entry != NULL) {
        Ns_CacheDeleteEntry(entry);
        entry = Ns_CacheNextEntry(&search);
        nflushed++;
    }
    ++cachePtr->stats.nflushed;

    return nflushed;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheFirstEntry --
 *
 *      Return a pointer to the first valid entry in the cache (in no
 *      particular order).
 *
 * Results:
 *      A pointer to said entry, or NULL if no valid entries.
 *
 * Side effects:
 *      Expired entries are flushed, concurrent updates are skipped.
 *
 *----------------------------------------------------------------------
 */
Ns_Entry *
Ns_CacheFirstEntry(Ns_Cache *cache, Ns_CacheSearch *search)
{
    return Ns_CacheFirstEntryT(cache, search, NULL);
}

Ns_Entry *
Ns_CacheFirstEntryT(Ns_Cache *cache, Ns_CacheSearch *search, const Ns_CacheTransactionStack *transactionStackPtr)
{
    Cache               *cachePtr = (Cache *) cache;
    const Tcl_HashEntry *hPtr;
    Ns_Entry            *result = NULL;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(search != NULL);

    Ns_GetTime(&search->now);
    hPtr = Tcl_FirstHashEntry(&cachePtr->entriesTable, &search->hsearch);
    while (hPtr != NULL) {
        Ns_Entry  *entry = Tcl_GetHashValue(hPtr);

        if (Ns_CacheGetValueT(entry, transactionStackPtr) != NULL) {
            if (!Expired((Entry *) entry, &search->now)) {
                result = entry;
                break;
            }
            ++cachePtr->stats.nexpired;
            Ns_CacheDeleteEntry(entry);
        }
        hPtr = Tcl_NextHashEntry(&search->hsearch);
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheCommitEntries --
 *
 *      Commit all cache entries at the end of a successful transaction.
 *
 * Results:
 *      number of committed entries.
 *
 * Side effects:
 *      Storing value like for transaction less cache entries.
 *
 *----------------------------------------------------------------------
 */
unsigned long
Ns_CacheCommitEntries(Ns_Cache *cache, uintptr_t epoch)
{
    unsigned long  result;
    Cache         *cachePtr;

    NS_NONNULL_ASSERT(cache != NULL);

    cachePtr = (Cache *) cache;
    result = CacheTransaction(cachePtr, epoch, NS_TRUE);
    cachePtr->stats.ncommit += result;

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheRollbackEntries --
 *
 *      Rollback all cache entries at the end of a successful transaction.
 *
 * Results:
 *      Number of rolled-back entries.
 *
 * Side effects:
 *      Freeing unneeded entries.
 *
 *----------------------------------------------------------------------
 */
unsigned long
Ns_CacheRollbackEntries(Ns_Cache *cache, uintptr_t epoch)
{
    unsigned long  result;
    Cache         *cachePtr;

    NS_NONNULL_ASSERT(cache != NULL);

    cachePtr = (Cache *) cache;
    result = CacheTransaction(cachePtr, epoch, NS_FALSE);
    cachePtr->stats.nrollback += result;

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CacheTransaction --
 *
 *      Helper function to iterate over caches and either commit or roll back
 *      write operations in the caches.
 *
 * Results:
 *      Number of modified entries.
 *
 * Side effects:
 *      Potentially freeing unneeded entries.
 *
 *----------------------------------------------------------------------
 */
static unsigned long
CacheTransaction(Cache *cachePtr, uintptr_t epoch, bool commit)
{
    Ns_CacheSearch     search;
    Tcl_HashEntry     *hPtr;
    unsigned long      count = 0u;

    NS_NONNULL_ASSERT(cachePtr != NULL);

    hPtr = Tcl_FirstHashEntry(&cachePtr->uncommittedTable, &search.hsearch);
    while (hPtr != NULL) {
        Ns_Entry  *entry = Tcl_GetHashKey(&cachePtr->uncommittedTable, hPtr);
        Entry     *e = (Entry *) entry;

        if (e->value == NULL && e->transactionEpoch == epoch) {

            if (commit) {
                e->value = e->uncommittedValue;
                e->uncommittedValue = NULL;
                e->transactionEpoch = 0u;

                Tcl_DeleteHashEntry(hPtr);
            } else {
                ((Entry *) entry)->cachePtr->stats.nexpired++;
                /*
                 * Ns_CacheDeleteEntry() will try to delete the same hPtr of above.
                 */
                Ns_CacheDeleteEntry(entry);
            }
            count ++;
        }
        hPtr = Tcl_NextHashEntry(&search.hsearch);
    }
    return count;
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
 *      Pointer to next valid entry, or NULL when all entries visited.
 *
 * Side effects:
 *      Expired entries are flushed, concurrent updates skipped.
 *
 *----------------------------------------------------------------------
 */

Ns_Entry *
Ns_CacheNextEntry(Ns_CacheSearch *search)
{
    return Ns_CacheNextEntryT(search, NULL);
}

Ns_Entry *
Ns_CacheNextEntryT(Ns_CacheSearch *search, const Ns_CacheTransactionStack *transactionStackPtr)
{
    const Tcl_HashEntry  *hPtr;
    Ns_Entry             *result = NULL;

    NS_NONNULL_ASSERT(search != NULL);

    hPtr = Tcl_NextHashEntry(&search->hsearch);
    while (hPtr != NULL) {
        Ns_Entry *entry = Tcl_GetHashValue(hPtr);

        if (Ns_CacheGetValueT(entry, transactionStackPtr) != NULL) {
            if (!Expired((Entry *) entry, &search->now)) {
                result = entry;
                break;
            }
            ((Entry *) entry)->cachePtr->stats.nexpired++;
            Ns_CacheDeleteEntry(entry);
        }
        hPtr = Tcl_NextHashEntry(&search->hsearch);
    }
    return result;
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

    NS_NONNULL_ASSERT(cache != NULL);
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

    NS_NONNULL_ASSERT(cache != NULL);
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

    NS_NONNULL_ASSERT(cache != NULL);
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

Ns_ReturnCode
Ns_CacheWait(Ns_Cache *cache)
{
    NS_NONNULL_ASSERT(cache != NULL);
    return Ns_CacheTimedWait(cache, NULL);
}

Ns_ReturnCode
Ns_CacheTimedWait(Ns_Cache *cache, const Ns_Time *timePtr)
{
    Cache *cachePtr = (Cache *) cache;

    NS_NONNULL_ASSERT(cache != NULL);
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

    NS_NONNULL_ASSERT(cache != NULL);
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

    NS_NONNULL_ASSERT(cache != NULL);
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
 *      Pointer to current string value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_CacheStats(Ns_Cache *cache, Ns_DString *dest)
{
    const Cache    *cachePtr;
    unsigned long   count, hitrate;
    const Entry    *ePtr;
    Ns_CacheSearch  search;
    double          savedCost = 0.0;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(dest != NULL);

    cachePtr = (Cache *)cache;
    count = cachePtr->stats.nhit + cachePtr->stats.nmiss;
    hitrate = ((count != 0u) ? (cachePtr->stats.nhit * 100u) / count : 0u);

    ePtr = (Entry *)Ns_CacheFirstEntry(cache, &search);
    while (ePtr != NULL) {
        savedCost += ((double)ePtr->count * (double)ePtr->cost) / 1000000.0;
        ePtr = (Entry *)Ns_CacheNextEntry(&search);
    }

    return Ns_DStringPrintf(dest, "maxsize %lu size %lu entries %d "
               "flushed %lu hits %lu missed %lu hitrate %lu "
               "expired %lu pruned %lu commit %lu rollback %lu saved %.6f",
               (unsigned long) cachePtr->maxSize,
               (unsigned long) cachePtr->currentSize,
               cachePtr->entriesTable.numEntries, cachePtr->stats.nflushed,
               cachePtr->stats.nhit, cachePtr->stats.nmiss, hitrate,
			    cachePtr->stats.nexpired, cachePtr->stats.npruned,
                            cachePtr->stats.ncommit, cachePtr->stats.nrollback,
			    savedCost);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheResetStats --
 *
 *      Set all statictics to zero.
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
Ns_CacheResetStats(Ns_Cache *cache)
{
    Cache *cachePtr = (Cache *) cache;

    NS_NONNULL_ASSERT(cache != NULL);
    memset(&cachePtr->stats, 0, sizeof(cachePtr->stats));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheSetMaxSize, Ns_CacheGetMaxSize --
 *
 *      Set/get maxsize of the specified cache
 *
 * Results:
 *      Ns_CacheGetMaxSize() returns the maxsize.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CacheSetMaxSize(Ns_Cache *cache, size_t maxSize)
{
    NS_NONNULL_ASSERT(cache != NULL);

    ((Cache *) cache)->maxSize = maxSize;
}

size_t
Ns_CacheGetMaxSize(const Ns_Cache *cache)
{
    NS_NONNULL_ASSERT(cache != NULL);

    return ((const Cache *) cache)->maxSize;
}



/*
 *----------------------------------------------------------------------
 *
 * Expired --
 *
 *      Has the absolute ttl expired?
 *
 * Results:
 *      NS_TRUE if entry has expired, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
Expired(const Entry *ePtr, const Ns_Time *nowPtr)
{
    Ns_Time  now;
    bool     expired = NS_FALSE;

    NS_NONNULL_ASSERT(ePtr != NULL);

    if (unlikely(ePtr->expires.sec > 0)) {
        if (nowPtr == NULL) {
            Ns_GetTime(&now);
            nowPtr = &now;
        }
        if (Ns_DiffTime(&ePtr->expires, nowPtr, NULL) < 0) {
            expired = NS_TRUE;
        }
    }
    return expired;
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
    NS_NONNULL_ASSERT(ePtr != NULL);

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
    NS_NONNULL_ASSERT(ePtr != NULL);

    if (likely(ePtr->cachePtr->firstEntryPtr != NULL)) {
        ePtr->cachePtr->firstEntryPtr->prevPtr = ePtr;
    }
    ePtr->prevPtr = NULL;
    ePtr->nextPtr = ePtr->cachePtr->firstEntryPtr;
    ePtr->cachePtr->firstEntryPtr = ePtr;
    if (unlikely(ePtr->cachePtr->lastEntryPtr == NULL)) {
        ePtr->cachePtr->lastEntryPtr = ePtr;
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

