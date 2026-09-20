/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
 */


/*
 * cache.c --
 *
 *      Size and time limited caches.
 */

#include "nsd.h"

struct Cache;

typedef enum {
    CACHE_ENTRY_UPDATING = 0, /* Producer is computing the value. */
    CACHE_ENTRY_READY,        /* value is committed and visible. */
    CACHE_ENTRY_UNCOMMITTED   /* Visible only to the owning transaction. */
} CacheEntryState;

typedef enum {
    CACHE_ACQUIRE_UPDATE, /* This caller must produce the value. */
    CACHE_ACQUIRE_VALUE,  /* A value is visible to this caller. */
    CACHE_ACQUIRE_WAIT    /* Another producer/transaction owns it. */
} CacheAcquireResult;

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
    Ns_Time         expires;          /* Absolute TTL timeout. */
    size_t          size;
    int             cost;             /* cost to compute a single entry */
    CacheEntryState state;            /* Protected by cachePtr->lock. */
    size_t          count;            /* reuse count of this entry */
    void           *value;            /* Committed value in READY state */
    void           *uncommittedValue; /* Value in UNCOMMITTED state */
    uintptr_t       transactionEpoch; /* Owner of an uncommitted value */
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
    Ns_FreeProc   *freeProc;
    Ns_Mutex       lock;
    Ns_Cond        cond;
    Tcl_HashTable  entriesTable;
    uintptr_t      transactionEpoch;
    Tcl_HashTable  uncommittedTable;
    struct {
        unsigned long   nhit;       /* Successful gets. */
        unsigned long   nmiss;      /* Unsuccessful gets. */
        unsigned long   nexpired;   /* Unsuccessful gets due to entry expiry. */
        unsigned long   nflushed;   /* Explicit flushes by user code. */
        unsigned long   npruned;    /* Evictions due to size constraint. */
        unsigned long   ncommit;    /* number of commits. */
        unsigned long   nrollback;  /* number of rollback operations. */
        unsigned long   ncollision; /* Acquisitions that had to wait. */
        unsigned long   nwait;      /* Condition-variable wait calls. */
        unsigned long   ntimeout;   /* Acquisitions ending in timeout. */
    } stats;

    char name[1];

} Cache;


/*
 * Local functions defined in this file
 */

static bool Expired(const Entry *ePtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1);

static void Remove(Entry *ePtr)
    NS_GNUC_NONNULL(1);

static void Push(Entry *ePtr)
    NS_GNUC_NONNULL(1);

static unsigned long
CacheTransaction(Cache *cachePtr, uintptr_t epoch, bool commit)
    NS_GNUC_NONNULL(1);

static Entry *CacheAcquireEntryT(Cache *cachePtr, const char *key,
                                 const Ns_CacheTransactionStack *transactionStackPtr,
                                 CacheAcquireResult *resultPtr)
    NS_GNUC_NONNULL(1,2,4);

static bool TransactionContains(const Ns_CacheTransactionStack *transactionStackPtr,
                                uintptr_t transactionEpoch);

static bool EntryIsVisibleT(const Entry *ePtr,
                            const Ns_CacheTransactionStack *transactionStackPtr)
    NS_GNUC_NONNULL(1);

static void AssertEntryState(const Entry *ePtr)
    NS_GNUC_NONNULL(1);

static void Touch(Entry *ePtr)
    NS_GNUC_NONNULL(1);


/*
 * Local function definitions.
 */

/*
 *----------------------------------------------------------------------
 *
 * TransactionContains --
 *
 *      Check whether the specified transaction epoch occurs in the active
 *      portion of a cache transaction stack.  A NULL transaction stack is
 *      treated as containing no transaction epochs.
 *
 * Results:
 *      NS_TRUE when transactionEpoch is present in the stack, otherwise
 *      NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
TransactionContains(const Ns_CacheTransactionStack *transactionStackPtr,
                    uintptr_t transactionEpoch)
{
    unsigned int i;

    if (transactionStackPtr != NULL) {
        for (i = 0u; i < transactionStackPtr->depth; i++) {
            if (transactionStackPtr->stack[i] == transactionEpoch) {
                return NS_TRUE;
            }
        }
    }
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * EntryIsVisibleT --
 *
 *      Determine whether the value of a cache entry is visible from the
 *      supplied transaction stack.  Committed entries are always visible,
 *      uncommitted entries are visible only to a transaction containing the
 *      matching epoch, and entries being updated are never visible.
 *
 *      The caller must hold the lock of the cache containing the entry.
 *
 * Results:
 *      NS_TRUE when the entry has a value visible to the caller, otherwise
 *      NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
EntryIsVisibleT(const Entry *ePtr,
                const Ns_CacheTransactionStack *transactionStackPtr)
{
    bool visible;

    switch (ePtr->state) {
    case CACHE_ENTRY_READY:
        visible = NS_TRUE;
        break;

    case CACHE_ENTRY_UNCOMMITTED:
        visible = TransactionContains(transactionStackPtr,
                                      ePtr->transactionEpoch);
        break;

    case CACHE_ENTRY_UPDATING:
        visible = NS_FALSE;
        break;

    default:
        assert(false);
        visible = NS_FALSE;
        break;
    }

    return visible;
}

/*
 *----------------------------------------------------------------------
 *
 * AssertEntryState --
 *
 *      Verify that the entry state agrees with its value pointers,
 *      transaction epoch, size, and other state-dependent members.
 *      The caller must hold the cache lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May raise an assertion failure when the entry is inconsistent.
 *
 *----------------------------------------------------------------------
 */
static void
AssertEntryState(const Entry *ePtr)
{
    switch (ePtr->state) {
    case CACHE_ENTRY_UPDATING:
        assert(ePtr->value == NULL);
        assert(ePtr->uncommittedValue == NULL);
        assert(ePtr->transactionEpoch == 0u);
        assert(ePtr->size == 0u);
        break;

    case CACHE_ENTRY_READY:
        assert(ePtr->value != NULL);
        assert(ePtr->uncommittedValue == NULL);
        assert(ePtr->transactionEpoch == 0u);
        break;

    case CACHE_ENTRY_UNCOMMITTED:
        assert(ePtr->value == NULL);
        assert(ePtr->uncommittedValue != NULL);
        assert(ePtr->transactionEpoch != 0u);
        break;

    default:
        assert(false);
        break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Touch --
 *
 *      Move an entry to the most-recently-used position of its cache,
 *      unless it is already there.
 *
 *      The caller must hold the cache lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May update the cache LRU list.
 *
 *----------------------------------------------------------------------
 */
static inline void
Touch(Entry *ePtr)
{
    if (ePtr->cachePtr->firstEntryPtr != ePtr) {
        Remove(ePtr);
        Push(ePtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheCreateSz --
 *
 *      Create a cache with the specified name, hash key type, maximum
 *      size, and value free procedure.
 *
 *      The keys argument specifies the Tcl hash table key type, such as
 *      TCL_STRING_KEYS.  maxSize specifies the memory budget used for
 *      size-based pruning; a value of zero disables size-based pruning.
 *
 *      When freeProc is non-NULL, it is called whenever a stored value
 *      is replaced or removed from the cache.
 *
 * Results:
 *      A pointer to the newly allocated cache.
 *
 * Side effects:
 *      Allocates and initializes the cache, its synchronization objects,
 *      and its entry and transaction hash tables.
 *
 *----------------------------------------------------------------------
 */
Ns_Cache *
Ns_CacheCreateSz(const char *name, int keys, size_t maxSize, Ns_FreeProc *freeProc)
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

    /* Statistics counters are initialized by ns_calloc(). */

    Ns_MutexInit(&cachePtr->lock);
    Ns_MutexSetName2(&cachePtr->lock, "ns:cache", name);
    Ns_CondInit(&cachePtr->cond);

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
 * Ns_CacheFindEntry, Ns_CacheFindEntryT --
 *
 *      Find a cache entry having a value visible to the caller.
 *      Ns_CacheFindEntryT respects the supplied cache transaction stack;
 *      Ns_CacheFindEntry performs a nontransactional lookup.
 *
 *      Entries being updated and uncommitted entries owned by another
 *      transaction are treated as unavailable.  An expired visible entry
 *      is deleted rather than returned.
 *
 *      The caller must hold the cache lock.
 *
 * Results:
 *      A pointer to a visible, nonexpired cache entry, or NULL when no such
 *      entry is available.
 *
 * Side effects:
 *      Updates hit or miss statistics.  A returned entry is moved to the
 *      most-recently-used position and its reuse count is incremented.
 *      An expired visible entry is deleted.
 *
 *----------------------------------------------------------------------
 */
Ns_Entry *
Ns_CacheFindEntry(Ns_Cache *cache, const char *key)
{
    return Ns_CacheFindEntryT(cache, key, NULL);
}

Ns_Entry *
Ns_CacheFindEntryT(
    Ns_Cache *cache,
    const char *key,
    const Ns_CacheTransactionStack *transactionStackPtr)
{
    Cache               *cachePtr = (Cache *)cache;
    const Tcl_HashEntry *hPtr;
    Entry               *ePtr;
    Ns_Entry            *result = NULL;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    hPtr = Tcl_FindHashEntry(&cachePtr->entriesTable, key);

    if (unlikely(hPtr == NULL)) {
        ++cachePtr->stats.nmiss;

    } else {
        ePtr = Tcl_GetHashValue(hPtr);
        AssertEntryState(ePtr);

        if (!EntryIsVisibleT(ePtr, transactionStackPtr)) {
            ++cachePtr->stats.nmiss;

        } else if (unlikely(Expired(ePtr, NULL))) {
            Ns_CacheDeleteEntry((Ns_Entry *)ePtr);
            ++cachePtr->stats.nmiss;

        } else {
            ++cachePtr->stats.nhit;
            ++ePtr->count;
            Touch(ePtr);
            result = (Ns_Entry *)ePtr;
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheCreateEntry --
 *
 *      Create a cache entry or return an existing entry for the specified
 *      key without waiting.
 *
 *      A missing entry or an expired CACHE_ENTRY_READY entry is
 *      transitioned to CACHE_ENTRY_UPDATING.  In this case, newPtr is
 *      set to a nonzero value to indicate that the caller owns production
 *      of the replacement value.
 *
 *      A nonexpired CACHE_ENTRY_READY entry is returned with newPtr set
 *      to zero.  An entry already in CACHE_ENTRY_UPDATING or
 *      CACHE_ENTRY_UNCOMMITTED state is also returned with newPtr set to
 *      zero; in these cases, zero does not indicate that a value is
 *      available to the caller.
 *
 *      Callers requiring an available value should use
 *      Ns_CacheWaitCreateEntry() or Ns_CacheWaitCreateEntryT().
 *
 *      The caller must hold the cache lock, which remains held on return.
 *      If the lock is released while an entry remains in
 *      CACHE_ENTRY_UPDATING state, the caller must eventually publish a
 *      value or delete the entry under the lock and notify potential
 *      waiters.
 *
 * Results:
 *      A pointer to the created or existing cache entry.  newPtr is set
 *      to a nonzero value only when the caller acquired ownership of
 *      producing the value.
 *
 * Side effects:
 *      May allocate an entry, release an expired value, transition an
 *      entry to CACHE_ENTRY_UPDATING, and update cache statistics and
 *      the LRU list.
 *
 *----------------------------------------------------------------------
 */
Ns_Entry *
Ns_CacheCreateEntry(Ns_Cache *cache, const char *key, int *newPtr)
{
    Cache              *cachePtr = (Cache *)cache;
    CacheAcquireResult  acquireResult;
    Entry              *ePtr;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(newPtr != NULL);

    ePtr = CacheAcquireEntryT(cachePtr, key, NULL, &acquireResult);

    /*
     * CACHE_ACQUIRE_WAIT is reported as an existing entry. This preserves
     * the existing non-waiting API contract.
     */
    *newPtr = (acquireResult == CACHE_ACQUIRE_UPDATE);

    return (Ns_Entry *)ePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * CacheAcquireEntryT --
 *
 *      Find or create a cache entry and classify it relative to the supplied
 *      transaction stack.
 *
 *      A missing entry, or an expired entry visible to the caller, is
 *      transitioned to CACHE_ENTRY_UPDATING and returned with
 *      CACHE_ACQUIRE_UPDATE.  The caller then owns production of the new
 *      value.
 *
 *      A valid value visible to the caller is returned with
 *      CACHE_ACQUIRE_VALUE.  An entry being updated, or an uncommitted entry
 *      owned by another transaction, is returned with CACHE_ACQUIRE_WAIT.
 *      This function does not wait.
 *
 *      The caller must hold the cache lock.  The lock remains held on
 *      return.
 *
 * Results:
 *      A pointer to the cache entry.  The acquisition result is stored in
 *      resultPtr.
 *
 * Side effects:
 *      May allocate an entry, release an expired value, change an entry
 *      state, update cache statistics, and update the LRU list.
 *
 *----------------------------------------------------------------------
 */
static Entry *
CacheAcquireEntryT(Cache *cachePtr, const char *key,
                   const Ns_CacheTransactionStack *transactionStackPtr,
                   CacheAcquireResult *resultPtr)
{
    Tcl_HashEntry *hPtr;
    Entry         *ePtr;
    int            isNew;

    NS_NONNULL_ASSERT(cachePtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(resultPtr != NULL);

    hPtr = Tcl_CreateHashEntry(&cachePtr->entriesTable, key, &isNew);

    if (isNew != 0) {
        /*
         * A newly allocated entry is owned by this caller and immediately
         * represents an update in progress.
         */
        ePtr = ns_calloc(1u, sizeof(Entry));
        ePtr->hPtr = hPtr;
        ePtr->cachePtr = cachePtr;
        ePtr->state = CACHE_ENTRY_UPDATING;

        Tcl_SetHashValue(hPtr, ePtr);
        cachePtr->currentSize +=
            sizeof(Entry) + sizeof(Tcl_HashEntry) + strlen(key);

        Push(ePtr);
        ++cachePtr->stats.nmiss;

        *resultPtr = CACHE_ACQUIRE_UPDATE;

    } else {
        ePtr = Tcl_GetHashValue(hPtr);
        AssertEntryState(ePtr);

        if (!EntryIsVisibleT(ePtr, transactionStackPtr)) {
            /*
             * This includes:
             *
             * - an update being computed by another thread;
             * - an uncommitted value owned by another transaction.
             *
             * Do not update hit statistics or the LRU position here.
             */
            *resultPtr = CACHE_ACQUIRE_WAIT;

        } else if (unlikely(Expired(ePtr, NULL))) {
            /*
             * Only a value visible to this caller may be expired here.
             * In particular, do not expire another transaction's
             * uncommitted value.
             */
            ++cachePtr->stats.nexpired;
            Ns_CacheUnsetValue((Ns_Entry *)ePtr);
            Touch(ePtr);

            *resultPtr = CACHE_ACQUIRE_UPDATE;

        } else {
            /*
             * A committed value, or an uncommitted value belonging to
             * this caller's transaction, is available.
             */
            ++ePtr->count;
            ++cachePtr->stats.nhit;
            Touch(ePtr);

            *resultPtr = CACHE_ACQUIRE_VALUE;
        }
    }

    AssertEntryState(ePtr);
    return ePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheWaitCreateEntry, Ns_CacheWaitCreateEntryT --
 *
 *      Acquire a cache entry having a value available to the caller, or
 *      acquire ownership of producing such a value.
 *
 *      A missing entry or an expired entry visible to the caller is
 *      transitioned to CACHE_ENTRY_UPDATING, and newPtr is set to a nonzero
 *      value.  For an existing visible value, newPtr is set to zero.
 *
 *      When an entry is being updated, or has an uncommitted value not
 *      visible to the supplied transaction stack, wait until its state
 *      changes or the specified absolute timeout is reached.  A NULL
 *      timeoutPtr requests an indefinite wait.
 *
 *      Ns_CacheWaitCreateEntryT respects the supplied cache transaction
 *      stack.  Ns_CacheWaitCreateEntry performs a nontransactional
 *      acquisition.
 *
 *      The caller must hold the cache lock.  A condition wait may release
 *      and reacquire the lock, but the lock is held when the function
 *      returns.
 *
 * Results:
 *      A pointer to the acquired entry, or NULL when the wait times out or
 *      otherwise fails.  On success, newPtr indicates whether the caller
 *      owns production of the value.  On failure, newPtr is set to zero.
 *
 * Side effects:
 *      May wait on the cache condition variable, allocate an entry, release
 *      an expired value, change an entry state, update cache statistics,
 *      and update the LRU list.
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
Ns_CacheWaitCreateEntryT(
    Ns_Cache *cache,
    const char *key,
    int *newPtr,
    const Ns_Time *timeoutPtr,
    const Ns_CacheTransactionStack *transactionStackPtr)
{
    Cache              *cachePtr = (Cache *)cache;
    CacheAcquireResult  acquireResult;
    Entry              *ePtr;
    Ns_ReturnCode        waitStatus = NS_OK;
    unsigned int         waits = 0u;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(newPtr != NULL);

    *newPtr = 0;

    for (;;) {
        ePtr = CacheAcquireEntryT(cachePtr, key, transactionStackPtr,
                                  &acquireResult);

        if (acquireResult == CACHE_ACQUIRE_VALUE) {
            *newPtr = 0;
            break;
        }

        if (acquireResult == CACHE_ACQUIRE_UPDATE) {
            *newPtr = 1;
            break;
        }

        /*
         * CACHE_ACQUIRE_WAIT:
         *
         * The entry is either being updated or contains an uncommitted value
         * that is not visible to this transaction.
         *
         * Log at most once for this acquisition attempt.
         */
        if (waits == 0u) {
            if (timeoutPtr == NULL) {
                /*
                 * An abandoned producer can leave every colliding request
                 * blocked indefinitely.
                 */
                Ns_Log(Warning,
                       "ns_cache unbounded create-entry wait cache %s key '%s'",
                       cachePtr->name, key);

            } else if (Ns_LogSeverityEnabled(Debug)) {
                Ns_Time        relTime;
                const Ns_Time *relTimePtr;

                relTimePtr = Ns_RelativeTime(&relTime, timeoutPtr);
                Ns_Log(Debug,
                       "ns_cache create-entry collision cache %s "
                       "key '%s', timeout " NS_TIME_FMT,
                       cachePtr->name, key,
                       (int64_t)relTimePtr->sec, relTimePtr->usec);
            }
        }

        ++waits;
        waitStatus = Ns_CacheTimedWait(cache, timeoutPtr);
        if (waitStatus != NS_OK) {
            if (waitStatus == NS_TIMEOUT) {
                Ns_Log(Warning,
                       "ns_cache create-entry wait timed out cache %s "
                       "key '%s' after %u condition wait%s",
                       cachePtr->name, key, waits,
                       waits == 1u ? "" : "s");
            } else {
                Ns_Log(Error,
                       "ns_cache create-entry wait failed cache %s key '%s'",
                       cachePtr->name, key);
            }
            /*
             * Do not attempt another acquisition after a failed wait.
             */
            ePtr = NULL;
            break;
        }
    }

    if (waits != 0u) {
        ++cachePtr->stats.ncollision;
        cachePtr->stats.nwait += waits;

        if (waitStatus == NS_TIMEOUT) {
            ++cachePtr->stats.ntimeout;
        }
    }

    return (Ns_Entry *)ePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheName --
 *
 *      Gets the name of a cache.
 *
 * Results:
 *      Name as const char*.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_CacheName(const Ns_Cache *cache) {

    NS_NONNULL_ASSERT(cache != NULL);

    return ((const Cache*)cache)->name;
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

    ePtr = (const Entry *) entry;
    return Tcl_GetHashKey(&ePtr->cachePtr->entriesTable, ePtr->hPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetSize, Ns_CacheGetExpirey,
 * Ns_CacheGetTransactionEpoch, Ns_CacheGetReuse --
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

size_t
Ns_CacheGetReuse(const Ns_Entry *entry)
{
    NS_NONNULL_ASSERT(entry != NULL);
    return ((const Entry *) entry)->count;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetValue, Ns_CacheGetValueT --
 *
 *      Return the value of a cache entry. Ns_CacheGetValue() returns a
 *      value only when the entry is in CACHE_ENTRY_READY state.
 *      Ns_CacheGetValueT() additionally returns an uncommitted value
 *      when the entry's transaction epoch is contained in the supplied
 *      transaction stack.
 *
 *      The caller must hold the cache lock.
 *
 * Results:
 *      The value visible in the requested context, or NULL when the
 *      entry is being updated or its uncommitted value is not visible
 *      in that context.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
Ns_CacheGetValue(const Ns_Entry *entry)
{
    const Entry *ePtr = (const Entry *)entry;

    NS_NONNULL_ASSERT(entry != NULL);
    //AssertEntryState(ePtr);

    return ePtr->state == CACHE_ENTRY_READY ? ePtr->value : NULL;
}

void *
Ns_CacheGetValueT(
    const Ns_Entry *entry,
    const Ns_CacheTransactionStack *transactionStackPtr)
{
    const Entry *ePtr = (const Entry *)entry;
    void        *result = NULL;

    NS_NONNULL_ASSERT(entry != NULL);
    //AssertEntryState(ePtr);

    switch (ePtr->state) {
    case CACHE_ENTRY_READY:
        result = ePtr->value;
        break;

    case CACHE_ENTRY_UNCOMMITTED:
        if (TransactionContains(transactionStackPtr,
                                ePtr->transactionEpoch)) {
            result = ePtr->uncommittedValue;
        }
        break;

    case CACHE_ENTRY_UPDATING:
        break;
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheGetNrUncommittedEntries --
 *
 *      Return for a given cache the number of uncommitted entries.
 *
 * Results:
 *      positive integer, type align with Tcl Hash Tables
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
TCL_SIZE_T
Ns_CacheGetNrUncommittedEntries(const Ns_Cache *cache)
{
    const Cache *cachePtr;

    NS_NONNULL_ASSERT(cache != NULL);

    cachePtr = (const Cache *)cache;
    return cachePtr->uncommittedTable.numEntries;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheSetMaxsize --
 *
 *      Set the maximum size (memory budget) for a cache.
 *
 *      This function updates the cache's configured maximum size used for
 *      space-based eviction decisions. It is intended as a small accessor
 *      for code outside of cache.c (e.g., Tcl wrappers) that must keep
 *      wrapper-level configuration in sync with the underlying cache.
 *
 * Returns:
 *      The previous maximum size of the cache.
 *
 * Side Effects:
 *      Updates the cache's maxsize parameter. Callers must ensure proper
 *      locking to avoid races with concurrent cache operations.
 *
 *----------------------------------------------------------------------
 */
size_t
Ns_CacheSetMaxsize(Ns_Cache *cache, size_t size)
{
    size_t  oldSize;
    Cache  *cachePtr;

    NS_NONNULL_ASSERT(cache != NULL);

    cachePtr = (Cache*)cache;
    oldSize = cachePtr->maxSize;
    cachePtr->maxSize = size;
    return oldSize;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheSetValue, Ns_CacheSetValueSz --
 *
 *      Store a committed value in a cache entry.  Ns_CacheSetValueSz also
 *      records the supplied value size.  These functions do not notify cache
 *      waiters.  A caller publishing a value after releasing the cache lock
 *      must broadcast afterward.
 *
 *      The caller must hold the cache lock.  Ownership of value is
 *      transferred to the cache.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Replaces and releases any previous value, transitions the entry to
 *      CACHE_ENTRY_READY, updates cache size accounting, and may prune other
 *      committed entries.
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

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheSetValueExpires --
 *
 *      Replace the contents of a cache entry and store the supplied value,
 *      size, expiration time, computation cost, and optional transaction
 *      epoch.
 *
 *      When transactionEpoch is zero, the value is committed immediately
 *      and the entry is transitioned to CACHE_ENTRY_READY.  Otherwise the
 *      value is registered as pending for that transaction and the entry is
 *      transitioned to CACHE_ENTRY_UNCOMMITTED.
 *
 *      A maxSize value of zero uses the cache's configured size limit.
 *      A nonzero value also updates the configured cache size limit.
 *
 *      The caller must hold the cache lock.  Ownership of value is
 *      transferred to the cache.
 *
 * Results:
 *      1 when the value was stored as uncommitted, otherwise 0.
 *
 * Side effects:
 *      Releases any previous value, updates transaction bookkeeping and
 *      cache size accounting, and may prune committed entries to satisfy
 *      the cache size limit.
 *
 *----------------------------------------------------------------------
 */
int
Ns_CacheSetValueExpires(Ns_Entry *entry, void *value, size_t size,
                        const Ns_Time *timeoutPtr, int cost, size_t maxSize,
                        uintptr_t transactionEpoch)
{
    Entry *ePtr;
    Cache *cachePtr;
    int    result;

    NS_NONNULL_ASSERT(entry != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    ePtr = (Entry *)entry;
    cachePtr = ePtr->cachePtr;

    /*
     * This supports both publishing a newly computed value and replacing
     * an existing value.
     */
    Ns_CacheUnsetValue(entry);

    if (transactionEpoch == 0u) {
        ePtr->value = value;
        ePtr->state = CACHE_ENTRY_READY;
        result = 0;

    } else {
        Tcl_HashEntry *hPtr;
        int            isNew;

        ePtr->uncommittedValue = value;
        ePtr->transactionEpoch = transactionEpoch;
        ePtr->state = CACHE_ENTRY_UNCOMMITTED;

        hPtr = Tcl_CreateHashEntry(&cachePtr->uncommittedTable,
                                   (const char *)ePtr, &isNew);
        (void)hPtr;

        if (unlikely(isNew == 0)) {
            Ns_Log(Warning,
                   "cache %s: adding entry %p with key '%s' "
                   "multiple times to pending table",
                   cachePtr->name, (void *)ePtr,
                   Ns_CacheKey(entry));
        }
        result = 1;
    }

    ePtr->size = size;
    ePtr->cost = cost;
    ePtr->count = 1u;

    if (timeoutPtr != NULL) {
        ePtr->expires = *timeoutPtr;
    }

    cachePtr->currentSize += size;

    if (maxSize == 0u) {
        maxSize = cachePtr->maxSize;
    } else if (maxSize != cachePtr->maxSize) {
        cachePtr->maxSize = maxSize;
    }

    AssertEntryState(ePtr);

    /*
     * Pruning must stop at entries that are being updated or are not yet
     * committed.
     */
    while (cachePtr->currentSize > maxSize
           && cachePtr->lastEntryPtr != ePtr
           && cachePtr->lastEntryPtr->state == CACHE_ENTRY_READY) {
        Ns_CacheDeleteEntry((Ns_Entry *)cachePtr->lastEntryPtr);
        ++cachePtr->stats.npruned;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheUnsetValue --
 *
 *      Release the current committed or uncommitted value and transition the
 *      entry to CACHE_ENTRY_UPDATING.
 *
 *      For an uncommitted entry, remove its pending-transaction table entry
 *      and clear its transaction epoch.  Value-related size, expiration,
 *      cost, and reuse information is reset.
 *
 *      An entry left in CACHE_ENTRY_UPDATING represents an update in
 *      progress.  The caller must eventually publish a replacement value or
 *      delete the entry and notify potential waiters.
 *
 *      The caller must hold the cache lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates cache size accounting, may remove transaction bookkeeping,
 *      and invokes the cache free procedure for the previous value.
 *
 *----------------------------------------------------------------------
 */
void
Ns_CacheUnsetValue(Ns_Entry *entry)
{
    Entry         *ePtr;
    Cache         *cachePtr;
    Tcl_HashEntry *hPtr;
    void          *value = NULL;

    NS_NONNULL_ASSERT(entry != NULL);

    ePtr = (Entry *)entry;
    cachePtr = ePtr->cachePtr;

    AssertEntryState(ePtr);

    switch (ePtr->state) {
    case CACHE_ENTRY_READY:
        value = ePtr->value;
        ePtr->value = NULL;
        break;

    case CACHE_ENTRY_UNCOMMITTED:
        value = ePtr->uncommittedValue;
        ePtr->uncommittedValue = NULL;

        hPtr = Tcl_FindHashEntry(&cachePtr->uncommittedTable,
                                 (const char *)ePtr);
        assert(hPtr != NULL);
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }
        break;

    case CACHE_ENTRY_UPDATING:
        /*
         * A newly created entry has no value to release.
         */
        break;
    }

    if (value != NULL) {
        assert(cachePtr->currentSize >= ePtr->size);
        cachePtr->currentSize -= ePtr->size;
    }

    ePtr->value = NULL;
    ePtr->uncommittedValue = NULL;
    ePtr->transactionEpoch = 0u;
    ePtr->size = 0u;
    ePtr->cost = 0;
    ePtr->count = 0u;
    ePtr->expires.sec = 0;
    ePtr->expires.usec = 0;
    ePtr->state = CACHE_ENTRY_UPDATING;

    AssertEntryState(ePtr);

    /*
     * Set the entry state and members before invoking the callback, since
     * the free procedure may re-enter cache-related code.
     */
    if (value != NULL && cachePtr->freeProc != NULL) {
        (*cachePtr->freeProc)(value);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheFlushEntry --
 *
 *      Explicitly flush an entry from the cache. Unlike
 *      Ns_CacheDeleteEntry(), this function records the removal as an
 *      explicit flush. Entries in any state, including
 *      CACHE_ENTRY_UPDATING, may be flushed.
 *
 *      The caller must hold the cache lock. This function does not
 *      notify threads waiting on the cache condition variable; when
 *      removal can unblock such waiters, the caller must broadcast.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments the flush statistic and deletes the entry. Any stored
 *      value is released through the configured free procedure, and
 *      the entry pointer becomes invalid.
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

/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheDeleteEntry --
 *
 *      Remove a cache entry from the entry table and LRU list, release its
 *      committed or uncommitted value, remove any transaction bookkeeping,
 *      and free the entry.
 *
 *      Entries in any state, including CACHE_ENTRY_UPDATING, may be deleted.
 *      This function does not notify threads waiting for the entry; callers
 *      completing or aborting an update must perform the required
 *      notification.
 *
 *      The caller must hold the cache lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates cache size accounting and may invoke the cache free
 *      procedure.
 *
 *----------------------------------------------------------------------
 */
void
Ns_CacheDeleteEntry(Ns_Entry *entry)
{
    Entry      *ePtr;
    Cache      *cachePtr;
    const char *key;

    NS_NONNULL_ASSERT(entry != NULL);

    ePtr = (Entry *)entry;
    cachePtr = ePtr->cachePtr;

    key = Tcl_GetHashKey(&cachePtr->entriesTable, ePtr->hPtr);
    cachePtr->currentSize -=
        sizeof(Entry) + sizeof(Tcl_HashEntry) + strlen(key);

    Ns_CacheUnsetValue(entry);

    Remove(ePtr);
    Tcl_DeleteHashEntry(ePtr->hPtr);
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
 *      Commit or roll back all uncommitted entries belonging to the
 *      specified transaction epoch.
 *
 *      On commit, each matching entry is transitioned from
 *      CACHE_ENTRY_UNCOMMITTED to CACHE_ENTRY_READY and removed from the
 *      pending-transaction table.  On rollback, each matching entry is
 *      deleted.
 *
 *      The caller must hold the cache lock.  This function does not notify
 *      cache waiters; the caller must broadcast after completing the
 *      transaction operation.
 *
 * Results:
 *      The number of entries committed or rolled back.
 *
 * Side effects:
  *      Updates cache entries, transaction bookkeeping, and, on rollback,
 *      cache size accounting.  Rollback may invoke the cache free procedure.
 *
 *----------------------------------------------------------------------
 */
static unsigned long
CacheTransaction(Cache *cachePtr, uintptr_t epoch, bool commit)
{
    Ns_CacheSearch  search;
    Tcl_HashEntry  *hPtr;
    unsigned long   count = 0u;

    NS_NONNULL_ASSERT(cachePtr != NULL);

    hPtr = Tcl_FirstHashEntry(&cachePtr->uncommittedTable,
                              &search.hsearch);

    while (hPtr != NULL) {
        Ns_Entry *entry =
            (Ns_Entry *)Tcl_GetHashKey(&cachePtr->uncommittedTable,
                                       hPtr);
        Entry *ePtr = (Entry *)entry;

        AssertEntryState(ePtr);

        if (ePtr->state == CACHE_ENTRY_UNCOMMITTED
            && ePtr->transactionEpoch == epoch) {

            if (commit) {
                ePtr->value = ePtr->uncommittedValue;
                ePtr->uncommittedValue = NULL;
                ePtr->transactionEpoch = 0u;
                ePtr->state = CACHE_ENTRY_READY;

                Tcl_DeleteHashEntry(hPtr);
                AssertEntryState(ePtr);

            } else {
                /*
                 * Ns_CacheDeleteEntry() removes the same uncommitted-table
                 * entry through Ns_CacheUnsetValue().
                 */
                Ns_CacheDeleteEntry(entry);
            }

            ++count;
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

Ns_ReturnCode
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
 *      Append statistics about cache usage to Tcl_DString.
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
Ns_CacheStats(Ns_Cache *cache, Tcl_DString *dest)
{
    const Cache    *cachePtr;
    unsigned long   count, nrewait;
    const Entry    *ePtr;
    Ns_CacheSearch  search;
    double          savedCost = 0.0, hitrate;

    NS_NONNULL_ASSERT(cache != NULL);
    NS_NONNULL_ASSERT(dest != NULL);

    cachePtr = (Cache *)cache;
    count = cachePtr->stats.nhit + cachePtr->stats.nmiss;
    hitrate = ((count != 0u) ? ((double)cachePtr->stats.nhit * 100.0) / (double)count : 0.0);
    nrewait = cachePtr->stats.nwait - cachePtr->stats.ncollision;

    ePtr = (Entry *)Ns_CacheFirstEntry(cache, &search);
    while (ePtr != NULL) {
        savedCost += ((double)ePtr->count * (double)ePtr->cost) / 1000000.0;
        ePtr = (Entry *)Ns_CacheNextEntry(&search);
    }

    return Ns_DStringPrintf(dest, "maxsize %lu size %lu entries %" PRITcl_Size
                            " flushed %lu hits %lu missed %lu hitrate %.2f"
                            " expired %lu pruned %lu commit %lu rollback %lu saved %.6f"
                            " collisions %lu waits %lu rewaits %lu timeouts %lu",
                            (unsigned long)cachePtr->maxSize,
                            (unsigned long)cachePtr->currentSize,
                            cachePtr->entriesTable.numEntries,
                            cachePtr->stats.nflushed,
                            cachePtr->stats.nhit,
                            cachePtr->stats.nmiss,
                            hitrate,
                            cachePtr->stats.nexpired,
                            cachePtr->stats.npruned,
                            cachePtr->stats.ncommit,
                            cachePtr->stats.nrollback,
                            savedCost,
                            cachePtr->stats.ncollision,
                            cachePtr->stats.nwait,
                            nrewait,
                            cachePtr->stats.ntimeout);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CacheResetStats --
 *
 *      Set all statistics to zero.
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
 *      Has the absolute TTL expired?
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
 * Remove --
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
Remove(Entry *ePtr)
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
