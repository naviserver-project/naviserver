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


/* thread_taffinity.h - lightweight thread-affinity checks (C99, pthreads) */

#ifndef THREAD_AFFINITY_H
# define THREAD_AFFINITY_H

/* Turn on in debug/dev builds (e.g., via CFLAGS: -DNS_ENABLE_THREAD_AFFINITY=1) */
# ifndef NS_ENABLE_THREAD_AFFINITY
#  define NS_ENABLE_THREAD_AFFINITY 0
# endif

# if NS_ENABLE_THREAD_AFFINITY

typedef struct NsThreadAffinity {
  uintptr_t   owner;     /* Ns_ThreadId() value */
  int         has_owner; /* 0/1 */
  const char *label;     /* optional */
} NsThreadAffinity;

#  define NS_TA_DECLARE(name)  NsThreadAffinity name;

#  define NS_TA_INIT(obj, field, lbl) do {   \
    (obj)->field.owner     = 0;              \
    (obj)->field.has_owner = 0;              \
    (obj)->field.label     = (lbl);          \
  } while (0)

#  define NS_TA_HANDOFF(obj, field, lbl) do { \
    (obj)->field.owner     = Ns_ThreadId();   \
    (obj)->field.has_owner = 1;               \
    (obj)->field.label     = (lbl);           \
  } while (0)

#  define NS_TA_IS_OWNER(obj, field) \
    ((obj)->field.has_owner && (obj)->field.owner == Ns_ThreadId())

#  define NS_TA_ASSERT_HELD(obj, field) do {                                   \
    if (!(NS_TA_IS_OWNER(obj, field))) {                                       \
      Tcl_Panic("thread affinity violation for %s (%s): owner=%" PRIuPTR       \
                " current=%" PRIuPTR,                                          \
                #field, ((obj)->field.label ? (obj)->field.label : ""),        \
                (uintptr_t)((obj)->field.owner), (uintptr_t)Ns_ThreadId());    \
    }                                                                          \
  } while (0)

#else  /* checks disabled */

//#  define NS_TA_DECLARE(name)                 struct { int _unused; } name
#  define NS_TA_DECLARE(name)
#  define NS_TA_INIT(obj, field, lbl)         ((void)0)
#  define NS_TA_HANDOFF(obj, field, lbl)      ((void)0)
#  define NS_TA_IS_OWNER(obj, field)          (1)
#  define NS_TA_ASSERT_HELD(obj, field)       ((void)0)

#endif
#endif /* THREAD_AFFINITY_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
