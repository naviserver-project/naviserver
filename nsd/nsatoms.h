/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

#ifndef NSATOMS_H
#define NSATOMS_H

#include "ns.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Declarations for function attributes (if not provided by ns.h already).
 */
#ifndef NS_GNUC_CONST
# if defined(__GNUC__) || defined(__clang__)
#  define NS_GNUC_CONST __attribute__((__const__))
# else
#  define NS_GNUC_CONST
# endif
#endif

#ifndef NS_GNUC_PURE
# if defined(__GNUC__) || defined(__clang__)
#  define NS_GNUC_PURE __attribute__((__pure__))
# else
#  define NS_GNUC_PURE
# endif
#endif

/*
 * Atom ids are integer indices into the global atom table.
 */
typedef int NsAtomId;

/*
 * Core atoms (core owned, stable ids).
 * Append-only to keep ids stable.
 */
typedef enum {
    NS_ATOM_EMPTY = 0,
    NS_ATOM_TRUE,
    NS_ATOM_FALSE,
    NS_ATOM_NULL,
    NS_ATOM_ZERO,
    NS_ATOM_ONE,

    /* from tclhhtp.c and url.c */
    NS_ATOM_BODY,
    NS_ATOM_BODY_CHAN,
    NS_ATOM_DATA,
    NS_ATOM_ERROR,
    NS_ATOM_EXPIRE,
    NS_ATOM_FILE,
    NS_ATOM_FIRSTLINE,
    NS_ATOM_FLAGS,
    NS_ATOM_FRAGMENT,
    NS_ATOM_HEADERS,
    NS_ATOM_HOST,
    NS_ATOM_HTTPS,
    NS_ATOM_NAME,
    NS_ATOM_OUTPUTCHAN,
    NS_ATOM_PATH,
    NS_ATOM_PEER,
    NS_ATOM_PHRASE,
    NS_ATOM_PORT,
    NS_ATOM_PROTO,
    NS_ATOM_QUERY,
    NS_ATOM_RECEIVED,
    NS_ATOM_REPLYBODYSIZE,
    NS_ATOM_REPLYLENGTH,
    NS_ATOM_REPLYSIZE,
    NS_ATOM_REQUESTLENGTH,
    NS_ATOM_REQUESTS,
    NS_ATOM_RUNNING,
    NS_ATOM_SENDBODYSIZE,
    NS_ATOM_SENT,
    NS_ATOM_SLOT,
    NS_ATOM_SOCK,
    NS_ATOM_STATE,
    NS_ATOM_STATUS,
    NS_ATOM_TAIL,
    NS_ATOM_TASK,
    NS_ATOM_TIME,
    NS_ATOM_TUNNEL,
    NS_ATOM_URL,
    NS_ATOM_USERINFO,

    /* end marker */
    NS_ATOM__CORE_MAX
} NsCoreAtomId;

/*
 * Global atoms lifecycle and access.
 *
 * NsAtomInit() creates core atoms and opens the registry for module
 * registrations. NsAtomSeal() closes it.
 */
NS_EXTERN void         NsAtomCoreInit(void);
NS_EXTERN void         NsAtomSeal(void);
NS_EXTERN bool         NsAtomSealed(void) NS_GNUC_PURE;
NS_EXTERN void         NsAtomShutdown(void);

/*
 * Current number of registered global atoms (core + dynamically registered).
 */
NS_EXTERN NsAtomId     NsAtomMax(void) NS_GNUC_PURE;

/*
 * Atom accessors.
 */
NS_EXTERN Tcl_Obj     *NsAtomObj(NsAtomId id);
NS_EXTERN const char  *NsAtomName(NsAtomId id, TCL_SIZE_T *lenPtr);

/*
 * Register a new global atom during startup (before NsAtomSeal()).
 *
 * - If an atom with the same name already exists, returns its id.
 * - On success, *idPtr receives the (possibly existing) id.
 * - Rejects registrations after seal.
 *
 * len == -1 => strlen(name)
 */
NS_EXTERN Ns_ReturnCode
NsAtomRegister(const char *name, TCL_SIZE_T len, NsAtomId *idPtr)
    NS_GNUC_NONNULL(1,3);

/*
 * Specs for module-local atom tables.
 * Each entry either references a global atom (share), or defines its own
 * literal (module-owned).
 */
typedef struct NsAtomSpec {
    NsAtomId    globalId;   /* -1 => not a global atom */
    const char *name;       /* used when globalId == -1 */
    TCL_SIZE_T  len;        /* -1 => strlen(name) */
} NsAtomSpec;

NS_EXTERN Ns_ReturnCode
NsAtomsInit(const NsAtomSpec *specs, size_t nSpecs, Tcl_Obj **atoms)
    NS_GNUC_NONNULL(1,3);

NS_EXTERN void
NsAtomsFreeOwned(const NsAtomSpec *specs, size_t nSpecs, Tcl_Obj **atoms)
    NS_GNUC_NONNULL(1,3);

#ifdef __cplusplus
}
#endif

#endif /* NSATOMS_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
