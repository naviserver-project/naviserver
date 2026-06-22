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
    NS_ATOM_true,
    NS_ATOM_false,
    NS_ATOM_NULL,
    NS_ATOM_0,
    NS_ATOM_1,

    NS_ATOM_3,
    NS_ATOM_DASH_offset,
    NS_ATOM_DASH_size,
    NS_ATOM_ERROR,
    NS_ATOM_FILTER_RETURN,
    NS_ATOM_FORBIDDEN,
    NS_ATOM_IPv4,
    NS_ATOM_IPv6,
    NS_ATOM_OK,
    NS_ATOM_UNAUTHORIZED,
    NS_ATOM_absent,
    NS_ATOM_address,
    NS_ATOM_agreement,
    NS_ATOM_allocated_dynamic,
    NS_ATOM_allocated_static,
    NS_ATOM_alpn,
    NS_ATOM_argon2,
    NS_ATOM_assertions,
    NS_ATOM_authority,
    NS_ATOM_bits,
    NS_ATOM_body,
    NS_ATOM_body_chan,
    NS_ATOM_boundary,
    NS_ATOM_brotli,
    NS_ATOM_bytes,
    NS_ATOM_callback,
    NS_ATOM_capabilities,
    NS_ATOM_channel,
    NS_ATOM_charset,
    NS_ATOM_cipher,
    NS_ATOM_ciphertext,
    NS_ATOM_clientcert,
    NS_ATOM_code,
    NS_ATOM_compiler,
    NS_ATOM_complete,
    NS_ATOM_condition,
    NS_ATOM_crv,
    NS_ATOM_currentaddr,
    NS_ATOM_curve,
    NS_ATOM_data,
    NS_ATOM_defaultport,
    NS_ATOM_description,
    NS_ATOM_digest,
    NS_ATOM_digests,
    NS_ATOM_dns,
    NS_ATOM_driver,
    NS_ATOM_e,
    NS_ATOM_ec,
    NS_ATOM_ed25519,
    NS_ATOM_ed448,
    NS_ATOM_email,
    NS_ATOM_enabled,
    NS_ATOM_error,
    NS_ATOM_errors,
    NS_ATOM_exception,
    NS_ATOM_expire,
    NS_ATOM_expires,
    NS_ATOM_extraheaders,
    NS_ATOM_file,
    NS_ATOM_filename,
    NS_ATOM_fin,
    NS_ATOM_fingerprint,
    NS_ATOM_firstline,
    NS_ATOM_flags,
    NS_ATOM_formfallback,
    NS_ATOM_fragment,
    NS_ATOM_fragments,
    NS_ATOM_frame,
    NS_ATOM_framebuffer,
    NS_ATOM_group,
    NS_ATOM_gzip,
    NS_ATOM_handler,
    NS_ATOM_havedata,
    NS_ATOM_headers,
    NS_ATOM_headersVersion,
    NS_ATOM_headersVersionNumber,
    NS_ATOM_hmac,
    NS_ATOM_host,
    NS_ATOM_https,
    NS_ATOM_httpversion,
    NS_ATOM_inany,
    NS_ATOM_incomplete,
    NS_ATOM_ip,
    NS_ATOM_iso8859_DASH_1,
    NS_ATOM_issuer,
    NS_ATOM_kem,
    NS_ATOM_keytypes,
    NS_ATOM_libraryversion,
    NS_ATOM_location,
    NS_ATOM_major,
    NS_ATOM_maxentry,
    NS_ATOM_maxsize,
    NS_ATOM_minor,
    NS_ATOM_ml_DASH_dsa,
    NS_ATOM_ml_DASH_kem,
    NS_ATOM_module,
    NS_ATOM_n,
    NS_ATOM_name,
    NS_ATOM_notafter,
    NS_ATOM_notbefore,
    NS_ATOM_nr_dynamic,
    NS_ATOM_nr_static,
    NS_ATOM_okp,
    NS_ATOM_opcode,
    NS_ATOM_output,
    NS_ATOM_outputchan,
    NS_ATOM_partial,
    NS_ATOM_patch,
    NS_ATOM_path,
    NS_ATOM_payload,
    NS_ATOM_peer,
    NS_ATOM_pem,
    NS_ATOM_phrase,
    NS_ATOM_pool,
    NS_ATOM_port,
    NS_ATOM_preload,
    NS_ATOM_present,
    NS_ATOM_proc,
    NS_ATOM_proto,
    NS_ATOM_protocol,
    NS_ATOM_provider,
    NS_ATOM_proxied,
    NS_ATOM_public,
    NS_ATOM_query,
    NS_ATOM_raw,
    NS_ATOM_received,
    NS_ATOM_recverror,
    NS_ATOM_recvwait,
    NS_ATOM_remaining_days,
    NS_ATOM_replybodysize,
    NS_ATOM_replylength,
    NS_ATOM_replysize,
    NS_ATOM_request,
    NS_ATOM_requestlength,
    NS_ATOM_requests,
    NS_ATOM_requiresdigest,
    NS_ATOM_rsa,
    NS_ATOM_running,
    NS_ATOM_runtimeVersion,
    NS_ATOM_runtimeVersionNumber,
    NS_ATOM_san,
    NS_ATOM_scrypt,
    NS_ATOM_secret,
    NS_ATOM_securityBits,
    NS_ATOM_securityCategory,
    NS_ATOM_sendbodysize,
    NS_ATOM_sendbuffer,
    NS_ATOM_senderror,
    NS_ATOM_sendwait,
    NS_ATOM_sent,
    NS_ATOM_serial,
    NS_ATOM_server,
    NS_ATOM_servername,
    NS_ATOM_signature,
    NS_ATOM_size_dynamic,
    NS_ATOM_size_static,
    NS_ATOM_slot,
    NS_ATOM_sm2,
    NS_ATOM_sock,
    NS_ATOM_spooled,
    NS_ATOM_sslversion,
    NS_ATOM_start,
    NS_ATOM_state,
    NS_ATOM_stats,
    NS_ATOM_status,
    NS_ATOM_subject,
    NS_ATOM_suffix,
    NS_ATOM_system_malloc,
    NS_ATOM_tag,
    NS_ATOM_tail,
    NS_ATOM_task,
    NS_ATOM_tcl,
    NS_ATOM_thread,
    NS_ATOM_time,
    NS_ATOM_timeout,
    NS_ATOM_trusted,
    NS_ATOM_tunnel,
    NS_ATOM_type,
    NS_ATOM_unknown,
    NS_ATOM_unlimited,
    NS_ATOM_unprocessed,
    NS_ATOM_uri,
    NS_ATOM_url,
    NS_ATOM_user,
    NS_ATOM_userinfo,
    NS_ATOM_verified,
    NS_ATOM_verifyresult,
    NS_ATOM_version,
    NS_ATOM_with_deprecated,
    NS_ATOM_x,
    NS_ATOM_x25519,
    NS_ATOM_x448,
    NS_ATOM_y,

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
NsAtomsInit(const NsAtomSpec *specs, size_t nSpecs, NsAtomId *outIds)
    NS_GNUC_NONNULL(1,3);

NS_EXTERN void
NsAtomsFreeOwned(const NsAtomSpec *specs, size_t nSpecs, Tcl_Obj **atomsVec)
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
