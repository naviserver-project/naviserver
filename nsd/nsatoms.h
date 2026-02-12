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

    NS_ATOM_ADDRESS,
    NS_ATOM_ALLOCATED_DYNAMIC,
    NS_ATOM_ALLOCATED_STATIC,
    NS_ATOM_ALPN,
    NS_ATOM_ASSERTIONS,
    NS_ATOM_AUTHORITY,
    NS_ATOM_BODY,
    NS_ATOM_BODY_CHAN,
    NS_ATOM_BROTLI,
    NS_ATOM_BYTES,
    NS_ATOM_CALLBACK,
    NS_ATOM_CHANNEL,
    NS_ATOM_CIPHER,
    NS_ATOM_CODE,
    NS_ATOM_COMPILER,
    NS_ATOM_COMPLETE,
    NS_ATOM_CONDITION,
    NS_ATOM_CURRENTADDR,
    NS_ATOM_DATA,
    NS_ATOM_DEFAULTPORT,
    NS_ATOM_DRIVER,
    NS_ATOM_ERROR,
    NS_ATOM_ERRORS,
    NS_ATOM_EXCEPTION,
    NS_ATOM_EXPIRE,
    NS_ATOM_EXTRAHEADERS,
    NS_ATOM_FILE,
    NS_ATOM_FIN,
    NS_ATOM_FIRSTLINE,
    NS_ATOM_FLAGS,
    NS_ATOM_FRAGMENT,
    NS_ATOM_FRAGMENTS,
    NS_ATOM_FRAME,
    NS_ATOM_FRAMEBUFFER,
    NS_ATOM_GZIP,
    NS_ATOM_HANDLER,
    NS_ATOM_HAVEDATA,
    NS_ATOM_HEADERS,
    NS_ATOM_HOST,
    NS_ATOM_HTTPS,
    NS_ATOM_HTTPVERSION,
    NS_ATOM_INANY,
    NS_ATOM_INCOMPLETE,
    NS_ATOM_LIBRARYVERSION,
    NS_ATOM_LOCATION,
    NS_ATOM_MODULE,
    NS_ATOM_NAME,
    NS_ATOM_NR_DYNAMIC,
    NS_ATOM_NR_STATIC,
    NS_ATOM_OPCODE,
    NS_ATOM_OUTPUTCHAN,
    NS_ATOM_PARTIAL,
    NS_ATOM_PATH,
    NS_ATOM_PAYLOAD,
    NS_ATOM_PEER,
    NS_ATOM_PHRASE,
    NS_ATOM_POOL,
    NS_ATOM_PORT,
    NS_ATOM_PRELOAD,
    NS_ATOM_PROC,
    NS_ATOM_PROTO,
    NS_ATOM_PROTOCOL,
    NS_ATOM_PROXIED,
    NS_ATOM_PUBLIC,
    NS_ATOM_QUERY,
    NS_ATOM_RECEIVED,
    NS_ATOM_RECVERROR,
    NS_ATOM_RECVWAIT,
    NS_ATOM_REPLYBODYSIZE,
    NS_ATOM_REPLYLENGTH,
    NS_ATOM_REPLYSIZE,
    NS_ATOM_REQUEST,
    NS_ATOM_REQUESTLENGTH,
    NS_ATOM_REQUESTS,
    NS_ATOM_RUNNING,
    NS_ATOM_SENDBODYSIZE,
    NS_ATOM_SENDBUFFER,
    NS_ATOM_SENDERROR,
    NS_ATOM_SENDWAIT,
    NS_ATOM_SENT,
    NS_ATOM_SERVER,
    NS_ATOM_SERVERNAME,
    NS_ATOM_SIZE_DYNAMIC,
    NS_ATOM_SIZE_STATIC,
    NS_ATOM_SLOT,
    NS_ATOM_SOCK,
    NS_ATOM_SPOOLED,
    NS_ATOM_SSLVERSION,
    NS_ATOM_START,
    NS_ATOM_STATE,
    NS_ATOM_STATS,
    NS_ATOM_STATUS,
    NS_ATOM_SYSTEM_MALLOC,
    NS_ATOM_TAIL,
    NS_ATOM_TASK,
    NS_ATOM_TCL,
    NS_ATOM_THREAD,
    NS_ATOM_TIME,
    NS_ATOM_TRUSTED,
    NS_ATOM_TUNNEL,
    NS_ATOM_TYPE,
    NS_ATOM_UNPROCESSED,
    NS_ATOM_URL,
    NS_ATOM_USER,
    NS_ATOM_USERINFO,
    NS_ATOM_VERSION,
    NS_ATOM_WITH_DEPRECATED,

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
