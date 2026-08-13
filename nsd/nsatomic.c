/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

#include "../nsd/nsd.h"
#include "../nsd/nsatomic.h"

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32Init --
 *
 *      Initialize an atomic integer with the specified value. This
 *      function must be called before the object is made accessible to
 *      other threads. It is not intended to race with exchange, store,
 *      or other initialization operations on the same object.
 *
 *      On platforms without native atomic operations, this also
 *      initializes the mutex used by the fallback implementation.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes atomicPtr and, when necessary, its fallback
 *      synchronization state.
 *
 *----------------------------------------------------------------------
 */
void
Ns_AtomicUint32Init(Ns_AtomicUint32 *atomicPtr, uint32_t value)
{
#if defined(_MSC_VER)
    atomicPtr->value = (LONG)value;

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    __atomic_store_n(&atomicPtr->value, value, __ATOMIC_RELAXED);
#else
    Ns_MutexInit(&atomicPtr->lock);
    Ns_MutexSetName(&atomicPtr->lock, "atomic");
    atomicPtr->value = value;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32ExchangeRelaxed --
 *
 *      Atomically replace the value held by atomicPtr and return its
 *      previous value. Concurrent exchange and store operations on the
 *      same object participate in a single atomic modification order.
 *
 *      The operation has relaxed memory-ordering semantics: it provides
 *      atomicity for this object but does not publish, acquire, or order
 *      accesses to unrelated memory. Callers must use appropriate queue
 *      locks or other synchronization when the value is associated with
 *      additional shared state.
 *
 *      Some platform implementations, such as MSVC interlocked operations
 *      or the mutex fallback, may provide stronger ordering than required
 *      by this interface.
 *
 * Results:
 *      Returns the value held by atomicPtr immediately before the
 *      exchange.
 *
 * Side effects:
 *      Replaces the value stored in atomicPtr.
 *
 *----------------------------------------------------------------------
 */
uint32_t
Ns_AtomicUint32ExchangeRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t value)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedExchange(&atomicPtr->value,
                                              (LONG)value);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    return __atomic_exchange_n(&atomicPtr->value,
                               value,
                               __ATOMIC_RELAXED);

#else
    uint32_t previous;

    Ns_MutexLock(&atomicPtr->lock);
    previous = atomicPtr->value;
    atomicPtr->value = value;
    Ns_MutexUnlock(&atomicPtr->lock);

    return previous;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32StoreRelaxed --
 *
 *      Atomically store the specified value in atomicPtr.
 *
 *      The operation has relaxed memory-ordering semantics: it provides
 *      an indivisible store and participates in the modification order
 *      of this atomic object, but does not publish or order accesses to
 *      unrelated memory. Additional shared state requires its own
 *      synchronization.
 *
 *      Some platform implementations may provide stronger ordering than
 *      the relaxed semantics required by this interface.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Replaces the value stored in atomicPtr.
 *
 *----------------------------------------------------------------------
 */
void
Ns_AtomicUint32StoreRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t value)
{
#if defined(_MSC_VER)
    (void)InterlockedExchange(&atomicPtr->value, (LONG)value);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    __atomic_store_n(&atomicPtr->value,
                     value,
                     __ATOMIC_RELAXED);

#else
    Ns_MutexLock(&atomicPtr->lock);
    atomicPtr->value = value;
    Ns_MutexUnlock(&atomicPtr->lock);
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32LoadRelaxed --
 *
 *      Atomically read the current value of the specified atomic integer.
 *
 *      The operation has relaxed memory-ordering semantics: it provides
 *      an indivisible read participating in the modification order of
 *      this atomic object, but does not acquire, publish, or order accesses
 *      to unrelated memory. Callers must use queue locks or another
 *      synchronization mechanism when the value governs additional shared
 *      state.
 *
 *      The object must have been initialized with Ns_AtomicUint32Init()
 *      before this function is called.
 *
 * Results:
 *      Returns the current value of the atomic integer.
 *
 * Side effects:
 *      None for native atomic implementations. On platforms without native
 *      atomic operations, temporarily acquires the fallback mutex associated
 *      with the object.
 *
 *----------------------------------------------------------------------
 */
uint32_t
Ns_AtomicUint32LoadRelaxed(Ns_AtomicUint32 *atomicPtr)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedCompareExchange(&atomicPtr->value, 0, 0);
#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    return __atomic_load_n(&atomicPtr->value, __ATOMIC_RELAXED);
#else
    uint32_t value;

    Ns_MutexLock(&atomicPtr->lock);
    value = atomicPtr->value;
    Ns_MutexUnlock(&atomicPtr->lock);

    return value;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32FetchOrRelaxed --
 *
 *      Atomically replace the current value with the bitwise OR of the
 *      current value and the specified mask.
 *
 *      The operation has relaxed memory-ordering semantics. It provides
 *      atomicity and participates in the modification order of this
 *      object, but does not publish or order accesses to unrelated
 *      memory.
 *
 * Results:
 *      Returns the value immediately before the bitwise OR operation.
 *
 * Side effects:
 *      Updates the atomic integer. On platforms without native atomic
 *      operations, temporarily acquires the object's fallback mutex.
 *
 *----------------------------------------------------------------------
 */
uint32_t
Ns_AtomicUint32FetchOrRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t mask)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedOr(&atomicPtr->value, (LONG)mask);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    return __atomic_fetch_or(&atomicPtr->value,
                             mask,
                             __ATOMIC_RELAXED);

#else
    uint32_t previous;

    Ns_MutexLock(&atomicPtr->lock);
    previous = atomicPtr->value;
    atomicPtr->value |= mask;
    Ns_MutexUnlock(&atomicPtr->lock);

    return previous;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32FetchAndRelaxed --
 *
 *      Atomically replace the current value with the bitwise AND of the
 *      current value and the specified mask.
 *
 *      The operation has relaxed memory-ordering semantics. It provides
 *      atomicity and participates in the modification order of this
 *      object, but does not publish or order accesses to unrelated
 *      memory.
 *
 * Results:
 *      Returns the value immediately before the bitwise AND operation.
 *
 * Side effects:
 *      Updates the atomic integer. On platforms without native atomic
 *      operations, temporarily acquires the object's fallback mutex.
 *
 *----------------------------------------------------------------------
 */
uint32_t
Ns_AtomicUint32FetchAndRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t mask)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedAnd(&atomicPtr->value, (LONG)mask);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    return __atomic_fetch_and(&atomicPtr->value,
                              mask,
                              __ATOMIC_RELAXED);

#else
    uint32_t previous;

    Ns_MutexLock(&atomicPtr->lock);
    previous = atomicPtr->value;
    atomicPtr->value &= mask;
    Ns_MutexUnlock(&atomicPtr->lock);

    return previous;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32LoadAcquire --
 *
 *      Atomically read the current value of the specified atomic
 *      integer with acquire memory-ordering semantics.
 *
 *      When this operation observes a value written by a release store
 *      on the same object, all memory accesses preceding that release
 *      become visible to the calling thread. This operation is therefore
 *      suitable for consuming state published through an atomic flag.
 *
 *      The object must have been initialized with
 *      Ns_AtomicUint32Init() before this function is called.
 *
 * Results:
 *      Returns the current value of the atomic integer.
 *
 * Side effects:
 *      None for native atomic implementations. On platforms without
 *      native atomic operations, temporarily acquires the fallback
 *      mutex associated with the object.
 *
 *----------------------------------------------------------------------
 */
uint32_t
Ns_AtomicUint32LoadAcquire(Ns_AtomicUint32 *atomicPtr)
{
#if defined(_MSC_VER)
    /*
     * InterlockedCompareExchange provides ordering stronger than the
     * acquire semantics required by this interface.
     */
    return (uint32_t)InterlockedCompareExchange(&atomicPtr->value, 0, 0);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    return __atomic_load_n(&atomicPtr->value, __ATOMIC_ACQUIRE);

#else
    uint32_t value;

    /*
     * Lock acquisition and release provide ordering stronger than a
     * native acquire load.
     */
    Ns_MutexLock(&atomicPtr->lock);
    value = atomicPtr->value;
    Ns_MutexUnlock(&atomicPtr->lock);

    return value;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32StoreRelease --
 *
 *      Atomically store the specified value in the atomic integer with
 *      release memory-ordering semantics.
 *
 *      All memory accesses preceding this operation become visible to
 *      a thread that subsequently observes the stored value through an
 *      acquire operation on the same object. This operation is therefore
 *      suitable for publishing associated shared state through an atomic
 *      flag.
 *
 *      The object must have been initialized with
 *      Ns_AtomicUint32Init() before this function is called.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Replaces the value stored in atomicPtr. On platforms without
 *      native atomic operations, temporarily acquires the fallback
 *      mutex associated with the object.
 *
 *----------------------------------------------------------------------
 */
void
Ns_AtomicUint32StoreRelease(Ns_AtomicUint32 *atomicPtr, uint32_t value)
{
#if defined(_MSC_VER)
    /*
     * InterlockedExchange provides ordering stronger than the release
     * semantics required by this interface.
     */
    (void)InterlockedExchange(&atomicPtr->value, (LONG)value);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    __atomic_store_n(&atomicPtr->value, value, __ATOMIC_RELEASE);

#else
    /*
     * The mutex provides ordering stronger than a native release store.
     */
    Ns_MutexLock(&atomicPtr->lock);
    atomicPtr->value = value;
    Ns_MutexUnlock(&atomicPtr->lock);
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32FetchOrRelease --
 *
 *      Atomically replace the current value with the bitwise OR of the
 *      current value and the specified mask, returning the previous value.
 *
 *      The operation has release memory-ordering semantics. All memory
 *      accesses preceding this operation become visible to a thread that
 *      subsequently observes the updated value through an acquire operation
 *      on the same atomic object.
 *
 *      Some platform implementations, such as MSVC interlocked operations
 *      or the mutex fallback, may provide stronger ordering than required
 *      by this interface.
 *
 * Results:
 *      Returns the value held by atomicPtr immediately before the bitwise
 *      OR operation.
 *
 * Side effects:
 *      Updates the atomic integer. On platforms without native atomic
 *      operations, temporarily acquires the object's fallback mutex.
 *
 *----------------------------------------------------------------------
 */
uint32_t
Ns_AtomicUint32FetchOrRelease(Ns_AtomicUint32 *atomicPtr, uint32_t mask)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedOr(&atomicPtr->value, (LONG)mask);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    return __atomic_fetch_or(&atomicPtr->value,
                             mask,
                             __ATOMIC_RELEASE);

#else
    uint32_t previous;

    Ns_MutexLock(&atomicPtr->lock);
    previous = atomicPtr->value;
    atomicPtr->value |= mask;
    Ns_MutexUnlock(&atomicPtr->lock);

    return previous;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AtomicUint32FetchAndRelease --
 *
 *      Atomically replace the current value with the bitwise AND of the
 *      current value and the specified mask, returning the previous value.
 *
 *      The operation has release memory-ordering semantics. All memory
 *      accesses preceding this operation become visible to a thread that
 *      subsequently observes the updated value through an acquire operation
 *      on the same atomic object.
 *
 *      Some platform implementations, such as MSVC interlocked operations
 *      or the mutex fallback, may provide stronger ordering than required
 *      by this interface.
 *
 * Results:
 *      Returns the value held by atomicPtr immediately before the bitwise
 *      AND operation.
 *
 * Side effects:
 *      Updates the atomic integer. On platforms without native atomic
 *      operations, temporarily acquires the object's fallback mutex.
 *
 *----------------------------------------------------------------------
 */
uint32_t
Ns_AtomicUint32FetchAndRelease(Ns_AtomicUint32 *atomicPtr, uint32_t mask)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedAnd(&atomicPtr->value, (LONG)mask);

#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    return __atomic_fetch_and(&atomicPtr->value,
                              mask,
                              __ATOMIC_RELEASE);

#else
    uint32_t previous;

    Ns_MutexLock(&atomicPtr->lock);
    previous = atomicPtr->value;
    atomicPtr->value &= mask;
    Ns_MutexUnlock(&atomicPtr->lock);

    return previous;
#endif
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
