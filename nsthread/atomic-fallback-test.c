/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Build the same tests against a private mutex-backed implementation. */
#define NS_ATOMIC_MUTEX_FALLBACK 1
#define Ns_AtomicUint32Init Test_Ns_AtomicUint32Init
#define Ns_AtomicUint32Destroy Test_Ns_AtomicUint32Destroy
#define Ns_AtomicUint32ExchangeRelaxed Test_Ns_AtomicUint32ExchangeRelaxed
#define Ns_AtomicUint32StoreRelaxed Test_Ns_AtomicUint32StoreRelaxed
#define Ns_AtomicUint32LoadRelaxed Test_Ns_AtomicUint32LoadRelaxed
#define Ns_AtomicUint32FetchOrRelaxed Test_Ns_AtomicUint32FetchOrRelaxed
#define Ns_AtomicUint32FetchAndRelaxed Test_Ns_AtomicUint32FetchAndRelaxed
#define Ns_AtomicUint32LoadAcquire Test_Ns_AtomicUint32LoadAcquire
#define Ns_AtomicUint32StoreRelease Test_Ns_AtomicUint32StoreRelease
#define Ns_AtomicUint32FetchOrRelease Test_Ns_AtomicUint32FetchOrRelease
#define Ns_AtomicUint32FetchAndRelease Test_Ns_AtomicUint32FetchAndRelease
#define Ns_AtomicUint32FetchAddRelaxed Test_Ns_AtomicUint32FetchAddRelaxed
#define Ns_AtomicUint32FetchSubAcqRel Test_Ns_AtomicUint32FetchSubAcqRel
#define Ns_AtomicUint64Init Test_Ns_AtomicUint64Init
#define Ns_AtomicUint64Destroy Test_Ns_AtomicUint64Destroy
#define Ns_AtomicUint64FetchAddRelaxed Test_Ns_AtomicUint64FetchAddRelaxed
#define Ns_AtomicUint64LoadRelaxed Test_Ns_AtomicUint64LoadRelaxed

#include "atomic.c"
#include "atomictest.c"
