/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

#ifndef NSATOMIC_H
# define NSATOMIC_H


typedef struct Ns_AtomicUint32 {
# if defined(_MSC_VER)
    volatile LONG value;
# elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS)
    uint32_t value;
# else
    uint32_t value;
    Ns_Mutex lock;
# endif
} Ns_AtomicUint32;

/*
 * Atomic accessors.
 */
NS_EXTERN void     Ns_AtomicUint32Init(Ns_AtomicUint32 *atomicPtr, uint32_t value) NS_GNUC_NONNULL(1);
NS_EXTERN void     Ns_AtomicUint32Destroy(Ns_AtomicUint32 *atomicPtr) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32ExchangeRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t value) NS_GNUC_NONNULL(1);
NS_EXTERN void     Ns_AtomicUint32StoreRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t value) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32LoadRelaxed(Ns_AtomicUint32 *atomicPtr) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32FetchOrRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t mask) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32FetchAndRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t mask) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32LoadAcquire(Ns_AtomicUint32 *atomicPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void     Ns_AtomicUint32StoreRelease(Ns_AtomicUint32 *atomicPtr, uint32_t value) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32FetchOrRelease(Ns_AtomicUint32 *atomicPtr, uint32_t mask) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32FetchAndRelease(Ns_AtomicUint32 *atomicPtr, uint32_t mask) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32FetchAddRelaxed(Ns_AtomicUint32 *atomicPtr, uint32_t value) NS_GNUC_NONNULL(1);
NS_EXTERN uint32_t Ns_AtomicUint32FetchSubAcqRel(Ns_AtomicUint32 *atomicPtr, uint32_t value) NS_GNUC_NONNULL(1);
#endif /* NSATOMIC_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
