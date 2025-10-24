/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
 * Copyright (C) 2025 Gustaf Neumann
 */

/*
 *======================================================================
 * quic.c - HTTP/3 driver for NaviServer over OpenSSL QUIC + nghttp3
 *======================================================================
 *
 *
 * Purpose
 * -------
 * Implements the NaviServer "quic" driver to serve HTTP/3 over QUIC,
 * using OpenSSL’s QUIC APIs together with nghttp3. It wires the driver
 * into NaviServer’s I/O pipeline and provides the module entry points.
 *
 * Responsibilities
 * ----------------
 * - Module/bootstrap:
 *    * Register UDP/QUIC transport and driver callbacks (Listen, Accept,
 *      Recv, Send, Keep, Close, ConnInfo, driver thread).
 *    * Initialize server SSL_CTX with the QUIC server method.
 * - Event loop:
 *    * Run the QUIC driver thread which polls SSL* objects, calls
 *      SSL_handle_events(), accepts QUIC connections/streams, and
 *      schedules read/write work.
 * - Pollset management:
 *    * Track connections/streams, maintain event masks, ensure capacity,
 *      and sweep dead entries.
 * - QUIC transport:
 *    * Drive the handshake, manage server-initiated uni streams
 *      (control/QPACK), extract peer addresses, and handle shutdown.
 * - HTTP/3 (nghttp3):
 *    * Provide allocator hooks and callbacks (e.g., recv_settings,
 *      begin_headers, on_end_stream, on_acked_stream_data), and submit/
 *      resume streams for header/body transmission.
 * - Shared/resume integration:
 *    * Drain a per-connection "resume ring" (SharedState) to coalesce
 *      runnable SIDs and issue nghttp3_conn_resume_stream() nudges.
 *
 * Configuration & Requirements
 * ----------------------------
 * - Load the module in the NaviServer config (ns_section ... ns_param
 *   quic quic.so), and provide TLS credentials suitable for QUIC.
 * - Requires OpenSSL with QUIC support and a compatible nghttp3 version.
 *
 * High-level Structure
 * --------------------
 * - Type/constant definitions for connection/stream contexts and enums.
 * - OpenSSL helpers and diagnostics (message callback, stringifiers).
 * - QUIC transport helpers (handshake, uni-streams, shutdown).
 * - HTTP/3 scheduling & write step, including resume-ring draining.
 * - Pollset management utilities and diagnostics.
 * - NaviServer driver interface glue (Send/Recv/Keep/Close/ConnInfo).
 *
 * References
 * ----------
 * - QUIC in OpenSSL: https://www.openssl.org/docs/man3.2/man7/ossl-guide-quic.html
 * - nghttp3 types/callbacks: https://nghttp2.org/nghttp3/
 */


/*
 *
 * Usage:
 *
 *   Configure in the NaviServer config file:
 *
 *   ###############################################
 *      ns_section ns/servers/$server/modules {
 *          ns_param quic quic.so
 *      }
 *
 *      ns_section    ns/servers/$server/module/quic {
 *          ns_param https ns/module/https
 *      }
 *   ###############################################
 *
 *
 */

#include "../include/ns.h"
#include "../nsd/nsd.h"

#define NS_ENABLE_THREAD_AFFINITY 1
#include "thread-affinity.h"

NS_EXPORT int Ns_ModuleVersion = 1;
NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;

#if defined(HAVE_NGHTTP3) && defined(HAVE_OPENSSL_EVP_H)
#if OPENSSL_VERSION_NUMBER >= 0x40000000L

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "../nsd/nsopenssl.h"
#include "shared.h"
#include <nghttp3/nghttp3.h>

#if NGHTTP3_VERSION_NUM < 0x010800
# error "nghttp3 version 1.8.0 or newer are required for the used HTTP/3 APIs"
#endif

#define WRITE_STEP_MAX_VEC 8

#define OSSL_TRY(call)                                          \
    do {                                                        \
        unsigned long _e; ERR_clear_error();                    \
        (void)(call);                                           \
        _e = ERR_peek_error();                                  \
        if (_e) {                                               \
            const char *reason = ERR_reason_error_string(_e);   \
            Ns_Log(Warning, "OpenSSL after %s: %s", #call,      \
                   reason ? reason : "(no reason)");            \
        }                                                       \
    } while (0)

#define ERRNO_WOULDBLOCK(e) ((e) == EAGAIN || (EAGAIN != EWOULDBLOCK && (e) == EWOULDBLOCK))

/*
 * Local structs and typedefs
 */

/* io_state bits */
#define H3_IO_RX_FIN         0x01u /* peer finished read side */
#define H3_IO_TX_FIN         0x02u /* we concluded write side */
#define H3_IO_RESET          0x04u /* stream reset/error (either side) */
#define H3_IO_REQ_READY      0x10u /* ready to dispatch (set in callbacks) -> maybe -> StreamCtx or h3ssl or some ReqCtxxs */
#define H3_IO_REQ_DISPATCHED 0x20u /* already dispatched  -> maybe -> StreamCtx or h3ssl or some ReqCtxxs */

#define H3_IO_HAS(sc, m)      (((sc)->io_state & (m)) != 0)
#define H3_RX_CLOSED(sc)      (H3_IO_HAS(sc, H3_IO_RX_FIN) || H3_IO_HAS(sc, H3_IO_RESET))
#define H3_TX_CLOSED(sc)      (H3_IO_HAS(sc, H3_IO_TX_FIN) || H3_IO_HAS(sc, H3_IO_RESET))
#define H3_BOTH_CLOSED(sc)    (H3_IO_HAS(sc, H3_IO_RESET) || (H3_IO_HAS(sc, H3_IO_TX_FIN) && H3_IO_HAS(sc, H3_IO_RX_FIN)))
#define H3_TX_WRITABLE(sc)    (!H3_TX_CLOSED(sc))

#define MAXSSL_IDS     20
#define MAXURL        255
#define MAX_SEND_HDRS  64

#define H3_CONN_ERR_MASK   (SSL_POLL_EVENT_EC | SSL_POLL_EVENT_ECD | SSL_POLL_EVENT_ER | SSL_POLL_EVENT_EW)
#define H3_STREAM_ERR_MASK (SSL_POLL_EVENT_ER | SSL_POLL_EVENT_EW)

typedef enum {
    H3_STEP_WROTE,       // we pushed at least one frame into the QUIC stack
    H3_STEP_NEED_EVENT,  // flow‐control, need SSL/TLS events
    H3_STEP_DONE         // nothing left in nghttp3 to send right now
} h3step_t;

typedef enum { TX_IDLE, TX_HDR_ACCUM, TX_HDR_DONE, TX_BODY } tx_state_t;

typedef enum {
    DRAIN_NONE,        // no bytes read, no state change
    DRAIN_PROGRESS,    // consumed bytes / callbacks fired
    DRAIN_EOF,         // peer finished read side (FIN) and rx empty
    DRAIN_CLOSED,      // stream is now closed/removed
    DRAIN_ERROR        // fatal error on this stream
} H3DrainResultCode;


typedef enum {
    FEED_OK_PROGRESS,
    FEED_OK_BLOCKED,     /* rv==0 */
    FEED_EOF,            /* FIN delivered (or scheduled) */
    FEED_ERR
} H3FeedResultCode;


/* Flags the helper returns so the call site can set did_progress, etc. */
typedef enum {
    H3_DISCARD_NONE      = 0,
    H3_DISCARD_ADVANCED  = 1u << 0,  /* advanced nghttp3 offsets > 0 */
    H3_DISCARD_FIN       = 1u << 1   /* applied FIN at nghttp3 (and set sc->TX_FIN if present) */
} H3DiscardState;

typedef enum {
    H3_KIND_UNKNOWN = 0,
    H3_KIND_BIDI_REQ,          // client-initiated bidi request
    H3_KIND_CTRL,              // our control stream
    H3_KIND_QPACK_ENCODER,     // our QPACK encoder (write-only)
    H3_KIND_QPACK_DECODER,     // our QPACK decoder (read-only)
    H3_KIND_CLIENT_UNI         // client uni streams, no need to differentiate
} H3StreamKind;


typedef uint64_t (PollsetMaskProc)(uint64_t);

typedef struct {
    int               is_h3;
    struct StreamCtx *sc;
    SSL              *ssl;
} QuicSockCtx;

struct ssl_id {
    SSL *s;      /* the stream openssl uses in SSL_read(),  SSL_write etc */
    uint64_t id; /* the stream identifier the nghttp3 uses */
    int status;  /* 0 or one the below status and origin */
};

struct h3ssl {
    /* the main QUIC+TLS connection */
    //struct ssl_id ssl_ids[MAXSSL_IDS];
    SSL   *conn;

    /* unidirectional HTTP/3 streams: */
    SSL   *cstream;    // control stream (SETTINGS, HEADERS)
    SSL   *pstream;    // QPACK encoder stream
    SSL   *rstream;    // QPACK decoder stream

    SSL   *bidi_ssl;
    uint64_t bidi_sid;         /* the bidi stream ID for the request/response */
    uint64_t cstream_id;
    uint64_t pstream_id;
    uint64_t rstream_id;
};

typedef struct ConnCtx {
    nghttp3_conn   *h3conn;
    SSL            *listener_ssl;
    struct h3ssl    h3ssl;
    NsTLSConfig    *dc;
    Ns_Mutex        lock;
    size_t          pidx;
    NS_TA_DECLARE(affinity)   /* owned by the H3/QUIC thread */

    Tcl_HashTable   streams;        /* key = (long)sid */
    bool            handshake_done; /* handshake completed */
    bool            settings_seen;
    bool            wants_write;
    bool            expecting_send;     // set when request dispatched to app
    bool            conn_closed;
    int             last_sd; // intermediate, for debuggung in ossl_conn_maybe_log_first_shutdown
    SharedState     shared;  // stable pointer

    int             connection_state;    // 0=active, 1=closing, 2=closed

    // Server-initiated (local) unis for writing
    int64_t         qpack_enc_sid;   // Stream ID for QPACK encoder
    int64_t         qpack_dec_sid;   // Stream ID for QPACK decoder
    SSL            *qpack_enc_ssl;   // SSL stream for QPACK encoder
    SSL            *qpack_dec_ssl;   // SSL stream for QPACK decoder

    // Client-initiated (peer) unis we read from:
    SSL           *client_control_ssl;
    SSL           *client_qpack_enc_ssl;
    SSL           *client_qpack_dec_ssl;
    uint64_t       client_control_sid;
    uint64_t       client_qpack_enc_sid;
    uint64_t       client_qpack_dec_sid;

    uint64_t       client_max_bidi_streams;   // maximum number of client-bidi streams we've told nghttp3
    uint64_t       client_max_field_section_size; /* 0 means "unknown yet" */
} ConnCtx;


typedef struct StreamCtx {
    SSL         *ssl;
    ConnCtx     *cc;
    uint64_t     quic_sid;     /* the stream ID reported by the QUIC/transport stack */
    int64_t      h3_sid;       /* the stream ID as seen by the HTTP/3 library */
    Ns_Sock     *nsSock;
    size_t       pidx;
    Ns_Mutex     lock;         /* protects wants_write for this stream */
    bool         wants_write;  /* QUIC thread clears, others set */
    uint8_t      io_state;     /* state bitmask, init to 0 */

    H3StreamKind kind;
    bool         writable;     /* quick test for capability, not for readiness */
    bool         seen_readable;
    bool         seen_io;
    bool         close_when_drained;
    bool         eof_seen;     /* eof detected from data, prevents re-draining */
    bool         type_consumed;
    bool         ignore_uni;
    bool         tx_served_this_step;  /* used to avoid double submissions from the my_read_data */
    bool         response_allow_body;
    bool         response_has_non_zero_content_length;
    uint64_t     uni_type;

    /* collected pseudo fields */
    const char  *method;
    const char  *path;
    const char  *authority;
    const char  *scheme;

    bool         saw_host_header;   /* case-insensitive detection of Host header */
    bool         hdrs_submitted;    /* nghttp3_conn_submit_response() done */
    bool         hdrs_ready;        /* headers staged but not submitted yet */
    bool         response_submitted;
    bool         eof_sent;

    /* receive buffer */
    uint8_t *rx_hold;        /* fixed-capacity wire buffer */
    size_t   rx_cap;         /* capacity of rx_hold (e.g., 8192 or 16384) */
    size_t   rx_len;         /* bytes valid in rx_hold */
    size_t   rx_off;         /* next unread offset in rx_hold */
    bool     rx_fin_pending; /* deliver FIN when rx_hold is empty */
    size_t   rx_emitted_in_pass;  /* used to avoid double receives via on_recv_data */

    /* body queues for sending to the client: */
    nghttp3_data_reader data_reader;
    ChunkQueue tx_queued;    // Chunks ready to be sent but not yet presented
    ChunkQueue tx_pending;        // maintained in the h3 thread
    SharedStream sh;

    bool          flow_blocked;
    Tcl_DString   resp_nv_store;   // backing store for copied names/values
    nghttp3_nv   *resp_nv;         /* array pointing into resp_nv_store */
    size_t        resp_nvlen;      /* number of nv pairs */
    tx_state_t    tx_state;
} StreamCtx;

static nghttp3_callbacks h3_callbacks = {0};
static nghttp3_mem h3_mem;

/*
 * Local functions defined in this file.
 */

/*
 *----------------------------------------------------------------------
 * OpenSSL helpers
 *----------------------------------------------------------------------
 */
static void        ossl_conn_log_close_info(NsTLSConfig *dc, SSL *conn) NS_GNUC_NONNULL(1,2);
static bool        ossl_conn_maybe_log_first_shutdown(ConnCtx *cc, const char* label) NS_GNUC_NONNULL(1,2);
static void        ossl_stream_log_state(NsTLSConfig *dc, SSL *stream, const char *label) NS_GNUC_NONNULL(1,2,3);
static void        ossl_log_stream_and_conn_states(ConnCtx *cc, SSL *s, SSL *conn, int st_expect, const char *where) NS_GNUC_NONNULL(1,2);
static void        ossl_log_handshake_state(SSL *conn) NS_GNUC_NONNULL(1) NS_GNUC_UNUSED;
static void        ossl_log_error_detail(int err, const char *msg)  NS_GNUC_NONNULL(2) NS_GNUC_UNUSED;

/* OpenSSL String formatters */
static const char *ossl_alert_desc_str(unsigned char d);
static const char *ossl_hs_type_str(unsigned t);
static const char *ossl_content_type_str(int content_type);
static const char *ossl_quic_stream_state_str(int ss);
static const char *ossl_quic_stream_type_str(int ss);

/* OpenSSL Callbacks / exdata */
static void        ossl_msg_cb(int write_p, int UNUSED(version), int content_type,
                               const void *UNUSED(buf), size_t len, SSL *ssl, void *UNUSED(arg)) NS_GNUC_UNUSED;
static void        ossl_cc_exdata_free(void *parent, void *ptr, CRYPTO_EX_DATA *UNUSED(ad),
                                       int UNUSED(idx), long UNUSED(argl), void *UNUSED(argp));
static void        ossl_sc_exdata_free(void *parent, void *ptr, CRYPTO_EX_DATA *UNUSED(ad),
                                       int UNUSED(idx), long UNUSED(argl), void *UNUSED(argp));

/*
 *----------------------------------------------------------------------
 * QUIC transport layer
 *----------------------------------------------------------------------
 */
static int      quic_conn_drive_handshake(NsTLSConfig *dc, SSL *conn) NS_GNUC_NONNULL(1);
static void     quic_conn_enter_shutdown(ConnCtx *cc, const char *why) NS_GNUC_NONNULL(1);
static bool     quic_conn_has_live_requests(ConnCtx *cc) NS_GNUC_NONNULL(1);
static bool     quic_conn_can_be_freed(SSL *conn, uint64_t revents, ConnCtx *cc) NS_GNUC_NONNULL(1,3);
static bool     quic_conn_set_sockaddr(SSL *ssl, struct sockaddr *saPtr, socklen_t *saLen) NS_GNUC_NONNULL(1,2,3);
static int      quic_conn_open_server_uni_streams(ConnCtx *cc, struct h3ssl *h3ssl) NS_GNUC_NONNULL(1,2);

static void     quic_stream_accepted_null(ConnCtx *cc) NS_GNUC_NONNULL(1);
static bool     quic_stream_keeps_conn_alive(StreamCtx *sc) NS_GNUC_NONNULL(1);
static bool     quic_conn_stream_map_empty(ConnCtx *cc);
static bool     quic_conn_can_be_freed_postloop(SSL *conn, ConnCtx *cc) NS_GNUC_NONNULL(1,2);

/* QUIC Utilities */
static void     quic_udp_set_rcvbuf(int fd, size_t rcvbuf_bytes);
static size_t   quic_varint_len(uint8_t b0);
static uint64_t quic_varint_decode(const uint8_t *p, size_t n) NS_GNUC_NONNULL(1);
static SSL*     quic_sid_to_stream(ConnCtx *cc, uint64_t sid) NS_GNUC_NONNULL(1);

/* QUIC Event handling & dispatch */
static void     quic_conn_handle_ic(SSL *listener_ssl, Driver *drvPtr) NS_GNUC_NONNULL(1,2);
static bool     quic_conn_handle_e(ConnCtx *cc, SSL *conn, uint64_t revents) NS_GNUC_NONNULL(1,2);
static bool     quic_stream_handle_e(ConnCtx *cc, SSL *stream, uint64_t sid,
                                     uint64_t revents, uint64_t current_mask) NS_GNUC_NONNULL(1,2);
static bool     quic_stream_handle_r(ConnCtx *cc, SSL *stream) NS_GNUC_NONNULL(1,2);

/* QUIC Event diagnostics / string formatters */
static char    *DStringAppendSslPollEventFlags(Tcl_DString *dsPtr, uint64_t flags) NS_GNUC_NONNULL(1) NS_GNUC_UNUSED;


/*
 *----------------------------------------------------------------------
 * HTTP/3 connection-level scheduling
 *----------------------------------------------------------------------
 */
static bool     h3_conn_write_step(ConnCtx *cc) NS_GNUC_NONNULL(1);
static void     h3_conn_clear_wants_write_if_idle(ConnCtx *cc) NS_GNUC_NONNULL(1);
static bool     h3_conn_has_work(ConnCtx *cc) NS_GNUC_NONNULL(1);
static void     h3_conn_mark_wants_write(ConnCtx *cc, StreamCtx *sc, const char *why) NS_GNUC_NONNULL(1,2);
static void     h3_conn_maybe_raise_client_bidi_credit(ConnCtx *cc, uint64_t sid) NS_GNUC_NONNULL(1);

/* H3 headers */
static Ns_HeadersEncodeProc h3_stream_build_resp_headers;
static bool     h3_headers_is_invalid_response_field(const char *name, size_t nlen, const char *val, size_t vlen) NS_GNUC_NONNULL(1,3);
static size_t   h3_headers_field_section_size(const nghttp3_nv *nva, size_t nvlen) NS_GNUC_NONNULL(1);
static int      h3_headers_nv_append(Tcl_DString *store, nghttp3_nv **pnva, size_t *pnvlen, size_t *pnvcap,
                                     const char *name, size_t nlen, const char *val, size_t vlen) NS_GNUC_NONNULL(1,2,3,4,5,7);
static void     h3_headers_log_nv(const StreamCtx *sc, const nghttp3_nv *nva, size_t nvlen, const char *label) NS_GNUC_NONNULL(1,2);

/* H3 data flow */
static H3FeedResultCode  h3_stream_feed_pending(StreamCtx *sc, uint64_t sid) NS_GNUC_NONNULL(1);
static H3DrainResultCode h3_stream_read_into_hold(StreamCtx *sc, SSL *stream) NS_GNUC_NONNULL(1,2);
static void              h3_stream_advance_and_trim(StreamCtx *sc, int64_t sid, uint8_t *base, size_t nbytes) NS_GNUC_NONNULL(1,3);
static H3DiscardState    h3_stream_skip_write_and_trim(ConnCtx *cc, StreamCtx *sc, int64_t h3_sid,
                                                       nghttp3_vec *vecs, int nvec, int fin, const char *reason) NS_GNUC_NONNULL(1);
static nghttp3_ssize     h3_stream_read_data_cb(nghttp3_conn *UNUSED(conn), int64_t stream_id, nghttp3_vec *vecs,
                                                size_t veccnt, uint32_t *flags, void *conn_user_data, void *stream_user_data);

/* H3 submit/resume & lifecycle */
static int               h3_stream_submit_ready_headers(StreamCtx *sc) NS_GNUC_NONNULL(1);
static H3DrainResultCode h3_stream_drain(ConnCtx *cc, SSL *stream, uint64_t sid, const char *label) NS_GNUC_NONNULL(1);
static bool              h3_stream_maybe_finalize(StreamCtx *sc, const char *label) NS_GNUC_NONNULL(1,2);
static bool              h3_stream_can_free(const StreamCtx *sc) NS_GNUC_NONNULL(1);
static void              h3_stream_maybe_note_uni_type(StreamCtx *sc, SSL *stream, uint64_t sid) NS_GNUC_NONNULL(1,2);
static void              h3_conn_wake(NsTLSConfig *dc) NS_GNUC_NONNULL(1);
static int64_t           h3_stream_id(const StreamCtx *sc) NS_GNUC_NONNULL(1);

/* H3 response body management */
static bool              h3_response_allows_body(int status, const char *method) NS_GNUC_NONNULL(2);
static bool              h3_response_has_body_now(StreamCtx *sc) NS_GNUC_NONNULL(1);


/* H3 diagnostics / stringifiers */
static const char *      H3DrainResultCode_str(H3DrainResultCode dr);
static const char *      H3FeedResultCode_str(H3FeedResultCode fr);
static const char *      H3StreamKind_str(H3StreamKind kind);

/* H3 / nghttp3 allocator hooks */
static void *            h3_malloc_cb(size_t size, void *UNUSED(user_data));
static void              h3_free_cb(void *ptr, void *UNUSED(user_data));
static void *            h3_calloc_cb(size_t nmemb, size_t size, void *UNUSED(user_data));
static void *            h3_realloc_cb(void *ptr, size_t size, void *UNUSED(user_data));

/*
 *----------------------------------------------------------------------
 * Connection and Stream Contexts
 *----------------------------------------------------------------------
 */
static ConnCtx*        ConnCtxNew(NsTLSConfig *dc, SSL *conn) NS_GNUC_NONNULL(1,2);
static void            ConnCtxFree(ConnCtx *cc) NS_GNUC_NONNULL(1);
static void            ConnCtxPrintSidTable(ConnCtx *cc) NS_GNUC_NONNULL(1) NS_GNUC_UNUSED;

/* Stream context handling */
static void            StreamCtxInit(StreamCtx *sc) NS_GNUC_NONNULL(1);
static void            StreamCtxFree(StreamCtx *sc) NS_GNUC_NONNULL(1);
static StreamCtx*      StreamCtxFromSock(NsTLSConfig *dc, Ns_Sock *sock) NS_GNUC_NONNULL(1,2);
static Tcl_HashEntry*  StreamCtxLookup(Tcl_HashTable *ht, int64_t sid, int create) NS_GNUC_NONNULL(1);
static StreamCtx*      StreamCtxGet(ConnCtx *cc, int64_t sid, int create) NS_GNUC_NONNULL(1);
static StreamCtx*      StreamCtxRegister(ConnCtx *cc, SSL *s, uint64_t sid, H3StreamKind kind) NS_GNUC_NONNULL(1,2);
static void            StreamCtxUnregister(StreamCtx *sc)  NS_GNUC_NONNULL(1);
static void            StreamCtxRequireRxBuffer(StreamCtx *sc) NS_GNUC_NONNULL(1);
static bool            StreamCtxClaimDispatch(StreamCtx *sc) NS_GNUC_NONNULL(1);

static bool            StreamCtxIsServerUni(const StreamCtx *sc) NS_GNUC_NONNULL(1);
static bool            StreamCtxIsClientUni(const StreamCtx *sc) NS_GNUC_NONNULL(1);
static bool            StreamCtxIsBidi(const StreamCtx *sc) NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 * Pollset management
 *----------------------------------------------------------------------
 */
static void            PollsetInit(NsTLSConfig *dc) NS_GNUC_NONNULL(1);
static void            PollsetFree(NsTLSConfig *dc) NS_GNUC_NONNULL(1) NS_GNUC_UNUSED;

static inline void     PollsetEnsurePollCapacity(NsTLSConfig *dc) NS_GNUC_NONNULL(1);
static inline size_t   PollsetCount(const NsTLSConfig *dc) NS_GNUC_NONNULL(1);
static void            PollsetPrint(NsTLSConfig *dc, const char *prefix, bool skip)  NS_GNUC_NONNULL(1,2);

static PollsetMaskProc PollsetDefaultConnErrors;
static PollsetMaskProc PollsetDefaultStreamErrors;

static size_t          PollsetAdd(NsTLSConfig *dc, SSL *s, uint64_t events, PollsetMaskProc maskf, const char *label, H3StreamKind kind) NS_GNUC_NONNULL(1,2);
static size_t          PollsetAddConnection(NsTLSConfig *dc, SSL *s, uint64_t events) NS_GNUC_NONNULL(1,2);
static size_t          PollsetAddStream(NsTLSConfig *dc, SSL *s, uint64_t events, H3StreamKind kind) NS_GNUC_NONNULL(1,2);
static StreamCtx*      PollsetAddStreamRegister(ConnCtx *cc, SSL *s, H3StreamKind kind) NS_GNUC_NONNULL(1,2);

static inline size_t   PollsetGetSlot(NsTLSConfig *dc, SSL *s, const StreamCtx *sc) NS_GNUC_NONNULL(1,2);
static uint64_t        PollsetGetEvents(NsTLSConfig *dc, SSL *s, const StreamCtx *sc) NS_GNUC_NONNULL(1,2);
static void            PollsetSetEvents(NsTLSConfig *dc, SSL *s, const StreamCtx *sc, uint64_t events) NS_GNUC_NONNULL(1,2,3) NS_GNUC_UNUSED;
static inline uint64_t PollsetUpdateEvents(NsTLSConfig *dc, SSL *s, const StreamCtx *sc,
                                           uint64_t set_bits, uint64_t clear_bits) NS_GNUC_NONNULL(1,2);

static inline void     PollsetEnableRead(NsTLSConfig *dc, SSL *s, StreamCtx *sc) NS_GNUC_NONNULL(1,2) NS_GNUC_UNUSED;
static inline void     PollsetDisableRead(NsTLSConfig *dc, SSL *s, const StreamCtx *sc, const char *label) NS_GNUC_NONNULL(1,2);
static inline void     PollsetEnableWrite(NsTLSConfig *dc, SSL *s, StreamCtx *sc, const char *label) NS_GNUC_NONNULL(1,2);
static inline void     PollsetDisableWrite(NsTLSConfig *dc, SSL *s, StreamCtx *sc, const char *label) NS_GNUC_NONNULL(1,2);

static inline void     PollsetUpdateConnPollInterest(ConnCtx *cc) NS_GNUC_NONNULL(1);
static size_t          PollsetHandleListenerEvents(NsTLSConfig *dc) NS_GNUC_NONNULL(1);
static void            PollsetMarkDead(ConnCtx *cc, SSL *conn, const char *msg) NS_GNUC_NONNULL(1,2);
static void            PollsetSweep(NsTLSConfig *dc) NS_GNUC_NONNULL(1);
static void            PollsetConsolidate(NsTLSConfig *dc) NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 * NaviServer Interface
 *----------------------------------------------------------------------
 */
static void NsTimeToTimeval(const Ns_Time *src, struct timeval *dst) NS_GNUC_NONNULL(1,2);

static Ns_Set *SockEnsureReqHeaders(StreamCtx *sc) NS_GNUC_NONNULL(1);
static Ns_ReturnCode SockDispatchFinishedRequest(StreamCtx *sc)  NS_GNUC_NONNULL(1);

static Ns_ThreadProc QuicThread;
static Ns_DriverListenProc Listen;
static Ns_DriverAcceptProc Accept;
static Ns_DriverRecvProc Recv;
static Ns_DriverSendProc Send;
static Ns_DriverKeepProc Keep;
static Ns_DriverCloseProc Close;
static Ns_DriverConnInfoProc ConnInfo;


/*======================================================================
 * Function Implementations: OpenSSL helpers
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * ossl_conn_log_close_info --
 *
 *      Retrieve and log diagnostic details about a QUIC or TLS connection
 *      shutdown using OpenSSL’s `SSL_get_conn_close_info()` API.
 *      Provides insight into transport-level or application-level close
 *      events, including error codes, alert numbers, and textual reasons.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Emits diagnostic log messages via Ns_Log() at either Error or Notice level,
 *      depending on whether the connection was closed due to a transport
 *      failure or other cause.
 *
 *----------------------------------------------------------------------
 */
static void ossl_conn_log_close_info(NsTLSConfig *dc, SSL *conn)
{
    SSL_CONN_CLOSE_INFO cci;

    if (SSL_get_conn_close_info(conn, &cci, sizeof(cci)) == 1) {
        if (cci.flags & SSL_CONN_CLOSE_FLAG_TRANSPORT) {
            const char *class_str = (cci.error_code >= 0x100) ? "HTTP/3 (app)" : "QUIC transport";
            uint64_t ec = cci.error_code;
            if ((ec & 0xFF00u) == 0x0100u) {
                unsigned alert = (unsigned)(ec & 0xFFu); // 303->47

                Ns_Log(Error, "QUIC close: remote=%d class=CRYPTO_ERROR tls_alert=%u"
                       " (illegal_parameter=%d) reason='%s'",
                       !(cci.flags & SSL_CONN_CLOSE_FLAG_LOCAL), alert, alert==47,
                       cci.reason?cci.reason:"");
            } else {
                Ns_Log(Error,
                       "QUIC close: remote=%d class=%s code=0x%llx reason='%s'",
                       !(cci.flags & SSL_CONN_CLOSE_FLAG_LOCAL),
                       class_str,
                       (unsigned long long)cci.error_code,
                       cci.reason ? cci.reason : "");
            }
        } else {
            Ns_Log(Notice, "[%lld] conn_close_info: not a transport failure", (long long)dc->iter);
        }
    } else {
        Ns_Log(Notice, "[%lld] can't get conn_close_info", (long long)dc->iter);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_conn_maybe_log_first_shutdown --
 *
 *      Helper to detect and log the first QUIC connection shutdown.
 *      When the shutdown state changes from open to closed, logs
 *      transport or TLS close info and key stream states.
 *
 * Results:
 *      Returns NS_TRUE if this is the first shutdown; otherwise NS_FALSE.
 *
 *----------------------------------------------------------------------
 */
static bool ossl_conn_maybe_log_first_shutdown(ConnCtx *cc, const char *label) {
    NsTLSConfig *dc    = cc->dc;
    bool         fired = NS_FALSE;
    int          sd    = SSL_get_shutdown(cc->h3ssl.conn);

    if (sd != 0 && cc->last_sd == 0) {
        SSL_CONN_CLOSE_INFO cci;

        if (SSL_get_conn_close_info(cc->h3ssl.conn, &cci, sizeof(cci)) == 1) {
            if (cci.flags & SSL_CONN_CLOSE_FLAG_TRANSPORT) {
                ossl_conn_log_close_info(dc, cc->h3ssl.conn);
            } else {
                unsigned long e = ERR_peek_error();
                Ns_Log(Error, "[%lld] QUIC conn %p entering shutdown %s: state=%d "
                       "last_err_lib=%d reason=%d (%s)",
                       (long long)dc->iter, (void*)cc->h3ssl.conn, label, sd,
                       (int)ERR_GET_LIB(e), (int)ERR_GET_REASON(e),
                       ERR_reason_error_string(e));
            }
        }

        // Log per-stream high-level states for the usual suspects:
        ossl_stream_log_state(dc, cc->h3ssl.cstream, "server-ctrl");
        ossl_stream_log_state(dc, cc->h3ssl.pstream, "server-qpack-enc");
        ossl_stream_log_state(dc, cc->h3ssl.rstream, "server-qpack-dec");
        if (cc->h3ssl.bidi_sid != (uint64_t)-1 && cc->h3ssl.bidi_ssl != NULL) {
            ossl_stream_log_state(dc, cc->h3ssl.bidi_ssl, "client-req-0");
        }
        fired = NS_TRUE;
    }
    cc->last_sd = sd;

    return fired;
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_stream_log_state --
 *
 *      Helper to log the current state of a QUIC stream, including
 *      stream ID, type, read/write state, and internal I/O flags.
 *
 * Results:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void ossl_stream_log_state(NsTLSConfig *dc, SSL *stream, const char *label) {
    uint64_t   sid = SSL_get_stream_id(stream);
    StreamCtx *sc = SSL_get_ex_data(stream, dc->u.h3.sc_idx);

    if (!sc || !sc->ssl) {
        Ns_Log(Notice, "[%lld] %s sid=%llu: (no ctx/ssl)", (long long)dc->iter, label, (long long)sid);
        return;
    }
    Ns_Log(Notice, "[%lld] %s sid=%llu: type=%d rs=%d ws=%d io_state %.2x",
           (long long)dc->iter, label, (long long)sid, SSL_get_stream_type(stream),
           SSL_get_stream_read_state(stream), SSL_get_stream_write_state(stream),
           sc->io_state);
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_log_stream_and_conn_states --
 *
 *      Helper to log QUIC stream and connection state details for
 *      debugging. Reports mismatched read/write states against the
 *      expected value and includes stream type, shutdown state, and
 *      I/O flags. Used to track stream lifecycles and diagnose
 *      protocol-level anomalies.
 *
 * Results:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
ossl_log_stream_and_conn_states(ConnCtx *cc, SSL *s, SSL *conn, int st_expect, const char *where)
{
    NsTLSConfig *dc = cc->dc;
    int64_t      sid = (int64_t)SSL_get_stream_id(s);
    int          st = SSL_get_stream_type(s);
    int          rs = SSL_get_stream_read_state(s);
    int          ws = SSL_get_stream_write_state(s);
    int          sd = conn ? SSL_get_shutdown(conn) : -1;
    StreamCtx   *sc = SSL_get_ex_data(s, dc->u.h3.sc_idx);
    bool         check_read, check_write;

    if (sc != NULL) {
        switch (sc->kind) {
        case H3_KIND_CTRL:
        case H3_KIND_QPACK_ENCODER:
        case H3_KIND_QPACK_DECODER:
            check_write = NS_TRUE;
            break;

        case H3_KIND_CLIENT_UNI:
            check_read = NS_TRUE;
            break;

        case H3_KIND_BIDI_REQ:
        case H3_KIND_UNKNOWN:
        default:
            check_read = NS_TRUE;
            check_write = NS_TRUE;
        }
    }

    if (sc == NULL) {
        Ns_Log(Notice,
               "[%lld] H3[%lld] %s: NO SC, ssl=%p type=%d (%s) rs=%d (%s) ws=%d (%s) conn.sd=%d",
               (long long)dc->iter, (long long)sid, where, (void*)s,
               st, ossl_quic_stream_type_str(st),
               rs, ossl_quic_stream_state_str(rs),
               ws, ossl_quic_stream_state_str(ws),
               sd);
    } else if (check_read && check_write && ( ws != st_expect || rs != st_expect)) {
        Ns_Log(Notice, "[%lld] H3[%lld] %s: ssl=%p BIDI read %s write %s io_state %.2x",
               (long long)dc->iter, (long long)sid, where, (void*)s,
               ossl_quic_stream_state_str(rs),
               ossl_quic_stream_state_str(ws),
               sc->io_state);
    } else if (check_write && ws != st_expect) {
        Ns_Log(Notice, "[%lld] H3[%lld] %s: ssl=%p write %s io_state %.2x",
               (long long)dc->iter, (long long)sid, where, (void*)s,
               ossl_quic_stream_state_str(ws),
               sc->io_state);
    } else if (check_read && rs != st_expect) {
        Ns_Log(Notice, "[%lld] H3[%lld] %s: ssl=%p read %s io_state %.2x",
               (long long)dc->iter, (long long)sid, where, (void*)s,
               ossl_quic_stream_state_str(rs),
               sc->io_state);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_log_handshake_state --
 *
 *      Logs the current TLS/QUIC handshake state of a connection,
 *      including its descriptive state name and numeric identifier.
 *      If ALPN negotiation succeeded, logs the negotiated protocol.
 *
 * Results:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void ossl_log_handshake_state(SSL *conn) {
    unsigned int         state = SSL_get_state(conn);
    const char          *name = SSL_state_string_long(conn);
    const unsigned char *alpn;
    unsigned int         len;

    Ns_Log(Notice, "Handshake state: %s (%u)", name, state);

    // Log ALPN status
    SSL_get0_alpn_selected(conn, &alpn, &len);
    if (len > 0) {
        Ns_Log(Notice, "Handshake state: Negotiated ALPN: %s", alpn);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_log_error_detail --
 *
 *      Helper to log detailed OpenSSL and system error information.
 *      Retrieves the most recent OpenSSL error, formats it into a
 *      readable string, and logs it together with errno and context.
 *      Clears the OpenSSL error queue after logging.
 *
 * Results:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
ossl_log_error_detail(int err, const char *msg)
{
    unsigned long osslerr = ERR_peek_error();
    char buf[256];

    buf[0] = 0;
    if (osslerr != 0) {
        ERR_error_string_n(osslerr, buf, sizeof(buf));
        Ns_Log(Error, "%s: err=%d errno=%d (%s) ossl=%lu (%s)",
               msg, err, errno, strerror(errno), osslerr, buf);
        ERR_clear_error();
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ossl_alert_desc_str, ossl_hs_type_str --
 *
 *      Utility functions that map numeric OpenSSL/TLS protocol codes
 *      to human-readable string descriptions for diagnostic logging.
 *
 *      ossl_alert_desc_str() returns a textual label for common TLS
 *      alert codes (e.g., "handshake_failure", "illegal_parameter").
 *
 *      ossl_hs_type_str() returns the descriptive name of a TLS/QUIC
 *      handshake message type (e.g., "ClientHello", "Certificate",
 *      "Finished"), aiding debugging and trace output.
 *
 *      ossl_quic_stream_state_str() maps QUIC stream state constants
 *      (e.g., SSL_STREAM_STATE_OK, SSL_STREAM_STATE_FINISHED) to
 *      readable identifiers.
 *
 *      ossl_quic_stream_type_str() converts QUIC stream direction
 *      or role types (e.g., bidi, read, write) into symbolic labels.
 *
 * Results:
 *      Each function returns a constant string describing the numeric
 *      code passed in. Intended for debug and trace logging only.
 *
 *----------------------------------------------------------------------
 */
static const char *ossl_alert_desc_str(unsigned char d) {
    switch(d){
    case 10: return "unexpected_message";
    case 20: return "bad_record_mac";
    case 40: return "handshake_failure";
    case 42: return "bad_certificate";
    case 47: return "illegal_parameter";
    case 70: return "protocol_version";
    case 80: return "internal_error";
    case 109: return "missing_extension";
    default: return "alert(?)";
    }
}

static const char *ossl_hs_type_str(unsigned t) {
    switch(t) {
    case 0x01: return "ClientHello";
    case 0x02: return "ServerHello";
    case 0x04: return "NewSessionTicket";   // post-handshake
    case 0x08: return "EncryptedExtensions";
    case 0x0b: return "Certificate";
    case 0x0c: return "ServerKeyExchange/TLS1.2";
    case 0x0d: return "CertificateRequest";
    case 0x0e: return "ServerHelloDone/TLS1.2";
    case 0x0f: return "CertificateVerify";
    case 0x14: return "Finished";
    case 0x18: return "KeyUpdate";           // forbidden in QUIC
    default:   return "Handshake(?)";
    }
}

static const char *ossl_quic_stream_state_str(int ss) {
    switch (ss) {
    case SSL_STREAM_STATE_NONE:         return "STREAM_STATE_NONE";
    case SSL_STREAM_STATE_OK:           return "STREAM_STATE_OK";
    case SSL_STREAM_STATE_WRONG_DIR:    return "STREAM_STATE_WRONG_DIR";
    case SSL_STREAM_STATE_FINISHED:     return "STREAM_STATE_FINISHED";
    case SSL_STREAM_STATE_RESET_LOCAL:  return "STREAM_STATE_RESET_LOCAL";
    case SSL_STREAM_STATE_RESET_REMOTE: return "STREAM_STATE_RESET_REMOTE";
    case SSL_STREAM_STATE_CONN_CLOSED:  return "STREAM_STATE_CONN_CLOSED";
    }
    return "STREAM_STATE_UNKNOWN";
}

static const char *ossl_quic_stream_type_str(int ss) {
    switch (ss) {
    case SSL_STREAM_TYPE_NONE:    return "STREAM_TYPE_NONE";
    case SSL_STREAM_TYPE_BIDI:    return "STREAM_TYPE_BIDI";
    case SSL_STREAM_TYPE_READ:    return "STREAM_TYPE_READ";
    case SSL_STREAM_TYPE_WRITE:   return "STREAM_TYPE_WRITE";
    }
    return "STREAM_TYPE_UNKNOWN";
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_content_type_str --
 *
 *      Return a human-readable description for a TLS or QUIC
 *      record-layer content_type value. This includes both
 *      the standard TLS record types defined by SSL3_RT_*
 *      (such as Handshake, Alert, or ApplicationData) and
 *      OpenSSL’s internal "pseudo" types used for debugging
 *      and tracing QUIC frames (e.g., QUICPacket, QUICFrameHeader).
 *
 * Results:
 *      Returns a pointer to a static string describing the
 *      provided content_type value, or a formatted "Unknown(n)"
 *      string for unrecognized types.
 *
 * Side effects:
 *      None (aside from using a static buffer for unknown values).
 *
 *----------------------------------------------------------------------
 */
static const char *ossl_content_type_str(int content_type)
{
    static char buf[32];

    switch (content_type) {
    case SSL3_RT_CHANGE_CIPHER_SPEC:
        return "ChangeCipherSpec";
    case SSL3_RT_ALERT:
        return "Alert";
    case SSL3_RT_HANDSHAKE:
        return "Handshake";
    case SSL3_RT_APPLICATION_DATA:
        return "ApplicationData";
    case SSL3_RT_HEADER:
        return "RecordHeader";
    case SSL3_RT_INNER_CONTENT_TYPE:
        return "InnerContentType";
    case SSL3_RT_QUIC_DATAGRAM:
        return "QUICDatagram";
    case SSL3_RT_QUIC_PACKET:
        return "QUICPacket";
    case SSL3_RT_QUIC_FRAME_FULL:
        return "QUICFrameFull";
    case SSL3_RT_QUIC_FRAME_HEADER:
        return "QUICFrameHeader";
    case SSL3_RT_QUIC_FRAME_PADDING:
        return "QUICFramePadding";
    default:
        snprintf(buf, sizeof(buf), "Unknown(%d)", content_type);
        return buf;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_msg_cb --
 *
 *      OpenSSL message callback for detailed TLS/QUIC protocol tracing.
 *      Invoked by OpenSSL whenever a record is sent or received when
 *      SSL_CTX_set_msg_callback() is enabled.
 *
 *      The function decodes and logs key message types:
 *        * TLS Handshake records - reports message direction, type,
 *          and length using ossl_hs_type_str().
 *        * Alerts - logs alert level and description using
 *          ossl_alert_desc_str().
 *        * QUIC/TLS 1.3 NewSessionTicket - parses and logs key fields
 *          (lifetime, age_add, nonce_len, ticket_len) to assist in
 *          diagnosing malformed tickets.
 *
 *      Special warnings are emitted for KeyUpdate and post-handshake
 *      CertificateRequest messages, which are invalid in QUIC.
 *      Any unrecognized record types are logged generically via
 *      ossl_content_type_str().
 *
 * Results:
 *      None. Information is emitted to the server log for debugging.
 *
 * Side effects:
 *      Diagnostic log output via Ns_Log(); no modification of OpenSSL state.
 *
 *----------------------------------------------------------------------
 */
static void ossl_msg_cb(int write_p, int UNUSED(version), int content_type,
                        const void *buf, size_t len, SSL *ssl, void *UNUSED(arg))
{
    const unsigned char *p = (const unsigned char *)buf;

    if (content_type == SSL3_RT_HANDSHAKE && len >= 1) {
        unsigned htype = p[0];
        const char *dir = write_p ? "Sent" : "Received";

        Ns_Log(Notice, "TLS %s: Handshake type=%u (%s) len=%zu",
               dir, htype, ossl_hs_type_str(htype), len);

        // If it *is* NewSessionTicket, dump the first fields to catch malformed encoding
        if (htype == 0x04 && len >= 5) {
            const unsigned char *q = p + 4;              // skip Handshake header

            // TLS1.3 NST structure starts at p[0]=type(1), p[1..3]=len(3)
            if (len >= 17) {
                uint32_t lifetime, age_add, nonce_len;

                lifetime = (uint32_t)((q[0]<<24)|(q[1]<<16)|(q[2]<<8)|q[3]); q+=4;
                age_add  = (uint32_t)((q[0]<<24)|(q[1]<<16)|(q[2]<<8)|q[3]); q+=4;
                nonce_len = *q++;
                Ns_Log(Notice, "  NST: lifetime=%u age_add=%u nonce_len=%u",
                       lifetime, age_add, nonce_len);
                if (4+4+1+nonce_len+2 <= len-4) {
                    unsigned ticket_len;

                    q += nonce_len;
                    ticket_len = (unsigned)(q[0]<<8)|q[1]; q+=2;
                    Ns_Log(Notice, "  NST: ticket_len=%u ext_remaining=%zu",
                           ticket_len, (size_t)(len - (size_t)(q - p) - ticket_len));
                }
            }
        }
        if (htype == 0x18) { // KeyUpdate (forbidden in QUIC)
            Ns_Log(Warning, "  WARNING: TLS KeyUpdate seen (QUIC forbids this)");
        }
        if (htype == 0x0d && write_p) { // server-side post-HS cert request (forbidden)
            Ns_Log(Warning, "  WARNING: post-handshake CertificateRequest seen (forbidden in QUIC)");
        }

    } else if(content_type == SSL3_RT_ALERT && len >= 2 ){
        const char *dir = write_p ? "Sent" : "Received";
        unsigned char level = p[0], desc = p[1];
        Ns_Log(Notice, "TLS %s: ALERT level=%u desc=%u (%s)",
               dir, level, desc, ossl_alert_desc_str(desc));

    } else {
        Ns_Log(Notice, "TLS %p %s: %s (%zu bytes)", (void*)ssl,
               write_p ? "Sent" : "Received",
               ossl_content_type_str(content_type),
               len);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ossl_cc_exdata_free, ossl_sc_exdata_free --
 *
 *      OpenSSL ex_data cleanup callbacks for QUIC connection and
 *      stream contexts. These functions are automatically invoked
 *      by OpenSSL when an SSL object (connection or stream) holding
 *      custom application data is freed.
 *
 *      ossl_cc_exdata_free() handles per-connection (ConnCtx)
 *      cleanup, while ossl_sc_exdata_free() handles per-stream
 *      (StreamCtx) cleanup. Both log the event and delegate actual
 *      resource release to ConnCtxFree() or StreamCtxFree().
 *
 * Results:
 *      None. The associated context memory is released if present.
 *
 * Side effects:
 *      May trigger logging and free related QUIC or HTTP/3 stream
 *      state.
 *
 *----------------------------------------------------------------------
 */

static void ossl_cc_exdata_free(void *parent, void *ptr, CRYPTO_EX_DATA *UNUSED(ad),
                                int UNUSED(idx), long UNUSED(argl), void *UNUSED(argp)) {
    StreamCtx *sc = ptr;
    if (sc != NULL) {
        Ns_Log(Notice, "ossl_cc_exdata_free calls StreamCtxFree %p parent %p", (void*)ptr, (void*)parent);
        ConnCtxFree(ptr);
    }
}

static void ossl_sc_exdata_free(void *parent, void *ptr, CRYPTO_EX_DATA *UNUSED(ad),
                                int UNUSED(idx), long UNUSED(argl), void *UNUSED(argp)) {
    StreamCtx *sc = ptr;
    if (sc != NULL) {
        Ns_Log(Notice, "ossl_sc_exdata_free calls StreamCtxFree %p parent %p", (void*)ptr, (void*)parent);
        StreamCtxFree(ptr);
    }
}

static void ConnCtxPrintSidTable(ConnCtx *cc) {
    Tcl_HashSearch hs;
    Tcl_HashEntry *e = Tcl_FirstHashEntry(&cc->streams, &hs);

    Ns_Log(Notice, "H3 SidTable for ConnCtx %p h3conn %p h3ssl %p",
           (void*)cc, (void*)cc->h3conn,  (void*)cc->h3ssl.conn);

    for (;  e != NULL; e = Tcl_NextHashEntry(&hs)) {
        int64_t    sid = PTR2LONG(Tcl_GetHashKey(&cc->streams, e));
        StreamCtx *sc = Tcl_GetHashValue(e);

        Ns_Log(Notice, "H3 ... sid %lld sc %p h3_sid %lld quic_sid %lld ssl %p nsSock %d",
               (long long)sid, (void*)sc,
               (long long)sc->h3_sid, (long long)sc->quic_sid, (void*)sc->ssl,
               sc->nsSock == NULL ? -1 : sc->nsSock->sock
               );
    }
}


/*======================================================================
 * Function Implementations: QUIC Transport Layer
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_drive_handshake --
 *
 *      Advance the QUIC/TLS handshake state for a given connection.
 *      This helper drives the OpenSSL handshake engine via
 *      SSL_do_handshake() and reports its progress or failure.
 *
 *      On success, it logs the handshake completion and the status
 *      of any early-data negotiation (accepted, rejected, or not sent).
 *      On partial progress (SSL_ERROR_WANT_READ / WANT_WRITE),
 *      it signals that further network I/O is required.
 *
 *      In case of failure, the function logs diagnostic details:
 *        * negotiated TLS group (if any)
 *        * number of configured extra chain certificates
 *        * connection close information (if QUIC transport error)
 *        * OpenSSL error detail via ossl_log_error_detail()
 *
 * Results:
 *      Returns 1 when the handshake completes successfully,
 *      0 when additional I/O is needed,
 *     -1 on hard failure.
 *
 * Side effects:
 *      Logs handshake and error information through Ns_Log().
 *      Clears and reads the OpenSSL error stack.
 *
 *----------------------------------------------------------------------
 */

static int quic_conn_drive_handshake(NsTLSConfig *dc, SSL *conn) {
    int ret, err;

    Ns_Log(Notice, "quic_conn_drive_handshake servername <%s>", SSL_get_servername(conn, TLSEXT_NAMETYPE_host_name));
    ERR_clear_error();

    // Now try to advance the handshake
    ret = SSL_do_handshake(conn);

    if (ret == 1) {
        int ed = SSL_get_early_data_status(conn);
        const char *eds =
            (ed == SSL_EARLY_DATA_ACCEPTED) ? "accepted" :
            (ed == SSL_EARLY_DATA_REJECTED) ? "rejected" :
            (ed == SSL_EARLY_DATA_NOT_SENT) ? "not-sent" : "unknown";

        Ns_Log(Notice, "[%lld] Handshake completed for %p (early-data status: %s)",
               (long long)dc->iter, (void*)conn, eds);
        return 1;
    }

    err = SSL_get_error(conn, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return 0;  // needs more network I/O
    }

    {
        /* group: shows if we accidentally negotiated a hybrid */
        long nid = SSL_get_shared_group(conn, 0);   /* first shared group */
        if (nid > 0) {
            Ns_Log(Notice, "[%lld] TLS group: %s", (long long)dc->iter, OBJ_nid2sn((int)nid));
        }

        {
            STACK_OF(X509)* extras = NULL;
            SSL_CTX_get_extra_chain_certs_only(SSL_get_SSL_CTX(conn), &extras);
            Ns_Log(Notice, "[%lld] TLS quic ctx extra chain count=%d",  (long long)dc->iter, extras ? sk_X509_num(extras) : 0);
        }

        /* QUIC close reason (transport/app) */
        ossl_conn_log_close_info(dc, conn);
    }

    // Hard failure
    ossl_log_error_detail(err, "quic_conn_drive_handshake");
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_enter_shutdown --
 *
 *      Perform an orderly teardown of a QUIC connection and all
 *      associated streams. This function ensures that shutdown
 *      is executed exactly once per connection and that all
 *      stream resources owned by the connection are released.
 *
 *      The function:
 *        * Marks the connection as closing (app-level state flag)
 *        * Disables further writes
 *        * Logs the shutdown reason
 *        * Triggers a graceful QUIC CONNECTION_CLOSE via SSL_shutdown()
 *          and processes pending QUIC transport events with
 *          SSL_handle_events()
 *        * Iterates through the pollset to locate and free all
 *          associated streams, disabling read/write interest and
 *          unregistering their StreamCtx mappings
 *        * Finally marks the connection’s own pollset entry as dead
 *          and releases the SSL handle
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees all active QUIC stream SSL objects owned by the
 *      connection. Removes the connection from pollset tracking.
 *      Emits diagnostic logs through Ns_Log().
 *
 *----------------------------------------------------------------------
 */
static void
quic_conn_enter_shutdown(ConnCtx *cc, const char *why)
{
    NsTLSConfig *dc = cc->dc;
    SSL         *conn;

    conn = cc->h3ssl.conn;
    if (conn == NULL) {
        return; /* already freed */
    }

    if (cc->connection_state == 0) {
        cc->connection_state = 1;            /* mark app-level closing */
    }
    NS_TA_ASSERT_HELD(cc, affinity);
    cc->wants_write = NS_FALSE;

    Ns_Log(Notice, "[%lld] H3D QUIC conn %p enter shutdown: %s",
           (long long)dc->iter, (void*)conn, (why ? why : "unspecified"));

    /* Try to emit CONNECTION_CLOSE; harmless if already closing */
    (void)SSL_shutdown(conn);
    (void)SSL_handle_events(conn);
    Ns_Log(Notice, "[%lld] SSL_handle_events in quic_conn_enter_shutdown conn %p => %d",
           (long long)dc->iter, (void*)conn, SSL_handle_events(conn));

    /* Remove all stream items owned by this connection */
    for (size_t i = 0; i < PollsetCount(dc); ++i) {
        ConnCtx *owner;
        SSL     *s = (SSL *)dc->u.h3.ssl_items.data[i];

        if (s == NULL) {
            continue;
        }

        owner = SSL_get_ex_data(s, dc->u.h3.cc_idx);
        if (owner != cc) {
            continue;
        }

        if (s != conn) {
            StreamCtx *sc = SSL_get_ex_data(s, dc->u.h3.sc_idx);

            if (sc) {
                /* Drop interest; unregister mapping; free */
                PollsetDisableRead(dc, s, sc, "quic_conn_enter_shutdown");
                PollsetDisableWrite(dc, s, sc, "quic_conn_enter_shutdown");
                StreamCtxUnregister(sc);
            }
            PollsetMarkDead(cc, s, "conn shutdown");
            SSL_free(s);
        }
    }

    /* Finally remove the connection item itself */
    PollsetMarkDead(cc, conn, "conn shutdown (self)");
    Ns_Log(Notice, "H3 quic_conn_enter_shutdown '%s' FREE conn %p", why, (void*)conn);
    SSL_free(conn);
}

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_has_live_requests --
 *
 *      Check whether a QUIC connection currently has any active
 *      streams that should keep it alive. The function iterates
 *      over all StreamCtx entries registered in the connection’s
 *      stream hash table and returns NS_TRUE if at least one stream
 *      qualifies as "live" according to quic_stream_keeps_conn_alive().
 *
 * Results:
 *      Returns NS_TRUE if any active stream keeps the connection alive,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
quic_conn_has_live_requests(ConnCtx *cc)
{
    Tcl_HashSearch it;
    Tcl_HashEntry *e = Tcl_FirstHashEntry(&cc->streams, &it);

    while (e) {
        StreamCtx *sc = (StreamCtx *)Tcl_GetHashValue(e);

        if (quic_stream_keeps_conn_alive(sc)) {
            return NS_TRUE;
        }
        e = Tcl_NextHashEntry(&it);
    }
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_can_be_freed --
 *
 *      Determine whether a QUIC connection can be safely released.
 *      This helper inspects the shutdown state, pending I/O events,
 *      and active request streams to ensure that the connection is
 *      fully quiescent before freeing its resources.
 *
 *      Specifically, the connection is eligible for cleanup when:
 *        * Both sides have completed the TLS/QUIC shutdown handshake,
 *        * There are no actionable poll events remaining, and
 *        * No live request streams are still keeping the connection alive.
 *
 * Results:
 *      Returns NS_TRUE if the connection can be safely freed,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      Logs a notice when the connection becomes freeable.
 *
 *----------------------------------------------------------------------
 */
static inline bool
quic_conn_can_be_freed(SSL *conn, uint64_t revents, ConnCtx *cc)
{
    const int  sd            = SSL_get_shutdown(conn);
    const bool both_shutdown = (sd & SSL_SENT_SHUTDOWN) && (sd & SSL_RECEIVED_SHUTDOWN);
    const bool no_open_req   = !quic_conn_has_live_requests(cc);
    const bool no_actionable =
        (revents & (SSL_POLL_EVENT_IC|SSL_POLL_EVENT_OSB|SSL_POLL_EVENT_OSU|
                    SSL_POLL_EVENT_ISB|SSL_POLL_EVENT_ISU|
                    SSL_POLL_EVENT_R|SSL_POLL_EVENT_W)) == 0;

    if (both_shutdown && no_actionable && no_open_req) {
        Ns_Log(Notice, "H3 quic_conn_can_be_freed %p", (void *)conn);
    }
    return both_shutdown && no_actionable && no_open_req;
}


/*
 *----------------------------------------------------------------------
 *
 * quic_conn_set_sockaddr --
 *
 *      Extract the remote peer address from an OpenSSL QUIC connection
 *      and populate a standard sockaddr structure with it.
 *
 *      This helper uses SSL_get_peer_addr() (available in OpenSSL 3.3+)
 *      to retrieve the BIO_ADDR of the peer, converts it into a
 *      sockaddr_in or sockaddr_in6, and fills in the family, address,
 *      and port fields accordingly. On BSD-derived systems, it also
 *      sets the sin_len/sin6_len field.
 *
 * Results:
 *      Returns NS_TRUE on success (valid address written to *saPtr and *saLen),
 *      or NS_FALSE if no valid address could be extracted.
 *
 * Side effects:
 *      Allocates and frees a temporary BIO_ADDR object.
 *      Clears *saPtr before writing into it.
 *
 *----------------------------------------------------------------------
 */
static bool
quic_conn_set_sockaddr(SSL *ssl, struct sockaddr *saPtr, socklen_t *saLen)
{
    BIO_ADDR      *peer = NULL;
    int            result = NS_FALSE;

    peer = BIO_ADDR_new();
    if (peer != NULL && SSL_get_peer_addr(ssl, peer) != 0) {
        unsigned char  addr[16];
        size_t         alen   = 0;
        int            fam    = BIO_ADDR_family(peer);
        unsigned short port_n = port_n = BIO_ADDR_rawport(peer);

        memset(saPtr, 0, sizeof(struct sockaddr_storage));
        if (fam == AF_INET && BIO_ADDR_rawaddress(peer, addr, &alen) && alen == 4) {
            saPtr->sa_family = AF_INET;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
            {
                uint8_t len8 = (uint8_t)sizeof(struct sockaddr_in);
                memcpy((char *)saPtr + offsetof(struct sockaddr_in, sin_len), &len8, sizeof len8);
            }
#endif
            {
                const size_t off = offsetof(struct sockaddr_in, sin_addr);
                memcpy((char *)saPtr + off, addr, 4);
            }
            {
                const size_t off = offsetof(struct sockaddr_in, sin_port);
                memcpy((char *)saPtr + off, &port_n, sizeof port_n);
            }
            *saLen = (socklen_t)sizeof(struct sockaddr_in);
            result = NS_TRUE;

        } else if (fam == AF_INET6 && BIO_ADDR_rawaddress(peer, addr, &alen) && alen == 16) {
            saPtr->sa_family = AF_INET6;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
            {
                uint8_t len8 = (uint8_t)sizeof(struct sockaddr_in6);
                memcpy((char *)saPtr + offsetof(struct sockaddr_in6, sin6_len), &len8, sizeof len8);
            }
#endif
            {
                const size_t off = offsetof(struct sockaddr_in6, sin6_addr);
                memcpy((char *)saPtr + off, addr, 16);
            }
            {
                const size_t off = offsetof(struct sockaddr_in6, sin6_port);
                memcpy((char *)saPtr + off, &port_n, sizeof port_n);
            }
            *saLen = (socklen_t)sizeof(struct sockaddr_in6);
            result = NS_TRUE;
        }
    }
    if (peer != NULL) {
        BIO_ADDR_free(peer);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_open_server_uni_streams --
 *
 *      Create and register the HTTP/3 server-initiated unidirectional
 *      streams (control, QPACK encoder, QPACK decoder) on an existing
 *      QUIC connection, and bind them to the nghttp3 connection.
 *
 *      The function:
 *        - Allocates three uni streams via SSL_new_stream(…, SSL_STREAM_FLAG_UNI).
 *        - Verifies they are write-only (server-initiated uni streams).
 *        - Registers each stream with the pollset and creates StreamCtx
 *          (PollsetAddStreamRegister), recording their QUIC stream IDs
 *          into *h3ssl.
 *        - Binds the control stream with nghttp3_conn_bind_control_stream().
 *        - Binds the QPACK encoder/decoder streams with
 *          nghttp3_conn_bind_qpack_streams().
 *        - Kicks the I/O state machine (h3_conn_write_step, SSL_handle_events)
 *          to flush any pending frames.
 *
 * Arguments:
 *      cc     - Connection context holding OpenSSL and HTTP/3 state.
 *      h3ssl  - Per-connection HTTP/3/SSL bundle where the new streams
 *               and their IDs are stored.
 *      cc->h3conn (nghttp3_conn*) and h3ssl->conn (SSL*) must be valid.
 *
 * Results:
 *      Returns 0 on success. On failure, logs a warning/error, unregisters
 *      any partially created StreamCtx, frees any created SSL streams,
 *      resets the stored stream pointers/IDs in *h3ssl, and returns -1.
 *
 * Side effects:
 *      Allocates OpenSSL stream objects; registers them with internal
 *      pollset/mappings; updates *h3ssl with stream pointers and IDs;
 *      performs nghttp3 control/QPACK binding; may advance QUIC state
 *      via SSL_handle_events().
 *
 *----------------------------------------------------------------------
 */
static int
quic_conn_open_server_uni_streams(ConnCtx *cc, struct h3ssl *h3ssl)
{
    NsTLSConfig  *dc = cc->dc;
    nghttp3_conn *h3conn = cc->h3conn;
    SSL          *conn = h3ssl->conn;
    StreamCtx    *csc = NULL, *psc = NULL, *rsc = NULL;

    if (conn == NULL) {
        Ns_Log(Warning, "H3: quic_conn_open_server_uni_streams no connection");
        return -1;
    }

    h3ssl->conn    = conn;
    h3ssl->cstream = SSL_new_stream(conn, SSL_STREAM_FLAG_UNI);
    h3ssl->pstream = SSL_new_stream(conn, SSL_STREAM_FLAG_UNI);
    h3ssl->rstream = SSL_new_stream(conn, SSL_STREAM_FLAG_UNI);

    if (h3ssl->rstream == NULL|| h3ssl->pstream  == NULL|| h3ssl->cstream == NULL) {
        Ns_Log(Warning, "H3: quic_conn_open_server_uni_streams: could not open uni‑streams");
        goto cleanup_err;
    }


    {  // sanity test
        int t0 = SSL_get_stream_type(h3ssl->cstream);
        int t1 = SSL_get_stream_type(h3ssl->pstream);
        int t2 = SSL_get_stream_type(h3ssl->rstream);

        Ns_Log(Notice, "[%lld] H3 server unis: c=%d, p=%d, r=%d (expect WRITE=%d)",
               (long long)dc->iter, t0, t1, t2, SSL_STREAM_TYPE_WRITE);
        assert(t0 == SSL_STREAM_TYPE_WRITE);
        assert(t1 == SSL_STREAM_TYPE_WRITE);
        assert(t2 == SSL_STREAM_TYPE_WRITE);
    }
    ossl_conn_maybe_log_first_shutdown(cc, "quic_conn_open_server_uni_streams streams created");

    ERR_clear_error();

    csc = PollsetAddStreamRegister(cc, cc->h3ssl.cstream, H3_KIND_CTRL);
    psc = PollsetAddStreamRegister(cc, cc->h3ssl.pstream, H3_KIND_QPACK_ENCODER);
    rsc = PollsetAddStreamRegister(cc, cc->h3ssl.rstream, H3_KIND_QPACK_DECODER);

    if (csc == NULL|| psc  == NULL|| rsc== NULL) {
        Ns_Log(Warning, "H3: quic_conn_open_server_uni_streams: could not setup streams");
        goto cleanup_err;
    }

    h3ssl->cstream_id = csc->quic_sid;
    h3ssl->pstream_id = psc->quic_sid;
    h3ssl->rstream_id = rsc->quic_sid;

    /* Bind control first */
    if (nghttp3_conn_bind_control_stream(h3conn, (int64_t)h3ssl->cstream_id) != 0) {
        Ns_Log(Error, "H3: Failed to bind control stream");
        goto cleanup_err;
    }
    ossl_conn_maybe_log_first_shutdown(cc, "quic_conn_open_server_uni_streams cstream bound");

    /* Now bind QPACK (server’s local streams) */
    if (nghttp3_conn_bind_qpack_streams(h3conn,
                                        (int64_t)h3ssl->pstream_id,
                                        (int64_t)h3ssl->rstream_id) != 0) {
        Ns_Log(Warning, "H3 quic_conn_open_server_uni_streams: nghttp3_conn_bind_qpack_streams failed");
        goto cleanup_err;
    }
    ossl_conn_maybe_log_first_shutdown(cc, "quic_conn_open_server_uni_streams qpack bound");

    h3_conn_write_step(cc);
    SSL_handle_events(conn);
    Ns_Log(Notice, "[%lld] SSL_handle_events in quic_conn_open_server_uni_streams conn %p => %d",
           (long long)dc->iter, (void*)cc->h3ssl.conn, SSL_handle_events(conn));

    Ns_Log(Notice, "[%lld] H3 quic_conn_open_server_uni_streams: cstream %llu %p pstream %llu %p rstream %llu %p",
           (long long)dc->iter,
           (long long)h3ssl->cstream_id, (void*)h3ssl->cstream,
           (long long)h3ssl->pstream_id, (void*)h3ssl->pstream,
           (long long)h3ssl->rstream_id, (void*)h3ssl->rstream
           );
    return 0;

 cleanup_err:
    if (csc != NULL) {
        StreamCtxUnregister(csc);
    }
    if (psc != NULL) {
        StreamCtxUnregister(psc);
    }
    if (rsc != NULL) {
        StreamCtxUnregister(rsc);
    }
    if (h3ssl->rstream != NULL) {
        SSL_free(h3ssl->rstream);
    }
    if (h3ssl->pstream != NULL) {
        SSL_free(h3ssl->pstream);
    }
    if (h3ssl->cstream != NULL) {
        SSL_free(h3ssl->cstream);
    }
    h3ssl->rstream    = h3ssl->pstream    = h3ssl->cstream = NULL;
    h3ssl->rstream_id = h3ssl->pstream_id = h3ssl->cstream_id = (uint64_t)-1;

    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * quic_stream_accepted_null --
 *
 *      Handle cases where an attempt to accept a new QUIC stream returns
 *      NULL, i.e., no new stream is currently available or an error
 *      occurred during acceptance. This helper decodes the OpenSSL
 *      error condition and logs diagnostic information at appropriate
 *      severity levels.
 *
 *      The function distinguishes between benign "retry later" situations
 *      (SSL_ERROR_WANT_READ/WRITE/NONE), transient system conditions
 *      (EAGAIN, EINTR), and real protocol or transport failures
 *      (SSL_ERROR_SSL, SSL_ERROR_ZERO_RETURN, SSL_ERROR_SYSCALL, etc.).
 *
 * Arguments:
 *      cc - Connection context whose QUIC session (cc->h3ssl.conn)
 *           triggered a null stream accept.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Logs detailed messages describing the cause of the null accept,
 *      including OpenSSL and system-level error information.
 *      Marks the connection as closed (cc->conn_closed = NS_TRUE)
 *      when SSL_ERROR_ZERO_RETURN indicates remote shutdown.
 *
 *----------------------------------------------------------------------
 */
static void quic_stream_accepted_null(ConnCtx *cc)
{
    NsTLSConfig  *dc = cc->dc;
    unsigned long e;
    int           saved_errno = errno; /* capture before OpenSSL calls */
    int           aerr = SSL_get_error(cc->h3ssl.conn, 0);

    switch (aerr) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_NONE:
        /* No stream ready, nothing fatal */
        break;

    case SSL_ERROR_SSL:
        e = ERR_peek_error();
        Ns_Log(Warning, "[%lld] H3 accept: SSL protocol error: %s",
               (long long)dc->iter, ERR_reason_error_string(e));
        break;

    case SSL_ERROR_ZERO_RETURN:
        Ns_Log(Notice, "[%lld] H3 accept: QUIC connection closed (no more streams)",
               (long long)dc->iter);
        cc->conn_closed = NS_TRUE;
        break;

    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
        Ns_Log(Warning, "[%lld] H3 accept: unexpected WANT_CONNECT/ACCEPT "
               "(should not happen in QUIC!)", (long long)dc->iter);
        break;

    case SSL_ERROR_SYSCALL:
        e = ERR_peek_error();
        if (e != 0) {
            Ns_Log(Warning, "[%lld] H3 accept: SYSCALL with SSL error: %s",
                   (long long)dc->iter, ERR_reason_error_string(e));

        } else if (ERRNO_WOULDBLOCK(saved_errno) || saved_errno == EINTR) {
            Ns_Log(Debug, "[%lld] H3 accept: SYSCALL transient errno=%d (%s)",
                   (long long)dc->iter, saved_errno, strerror(saved_errno));

        } else if (saved_errno == 0) {
            /* Ambiguous in UDP/QUIC: treat as 'no stream now', not a hard close */
            Ns_Log(Debug, "[%lld] H3 accept: SYSCALL with errno=0 (ambiguous) -> retry later",
                   (long long)dc->iter);

        } else {
            Ns_Log(Warning, "[%lld] H3 accept: SYSCALL errno=%d (%s)",
                   (long long)dc->iter, saved_errno, strerror(saved_errno));
        }
        break;

    default:
        e = ERR_peek_error();
        Ns_Log(Warning, "[%lld] H3 accept: unexpected SSL error=%d (%s)",
               (long long)dc->iter, aerr,
               e ? ERR_reason_error_string(e) : "no details");
        ossl_log_error_detail(aerr, "set_incoming_stream_policy(conn)");

        break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * quic_stream_keeps_conn_alive --
 *
 *      Determine whether a given HTTP/3 stream should keep its parent
 *      QUIC connection alive. This is used to delay connection teardown
 *      until all active client-initiated bidirectional streams have
 *      fully completed and no pending data remains.
 *
 *      The function considers only H3_KIND_BIDI_REQ streams (client
 *      request streams). Unidirectional or control streams never
 *      influence connection liveness.
 *
 *      A stream is considered "alive" if either its read or write
 *      side remains open (SSL_STREAM_STATE_OK) or if it has pending
 *      queued data that has not yet been transmitted or acknowledged.
 *      Once both halves have cleanly finished (H3_IO_RX_FIN and
 *      H3_IO_TX_FIN set) and all buffers are drained, the stream no
 *      longer keeps the connection alive.
 *
 * Arguments:
 *      sc - Stream context to test.
 *
 * Results:
 *      Returns NS_TRUE if the stream should prevent connection teardown,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None (reads only stream state and queues).
 *
 *----------------------------------------------------------------------
 */
static inline bool
quic_stream_keeps_conn_alive(StreamCtx *sc)
{
    if (sc->ssl == NULL) {
        return NS_FALSE;

    } else if (sc->kind != H3_KIND_BIDI_REQ) {
        /* Only client-initiated bidi requests gate connection teardown. */
        return NS_FALSE;

    } else {

        int rs = SSL_get_stream_read_state(sc->ssl);
        int ws = SSL_get_stream_write_state(sc->ssl);
        const bool rx_open = ((sc->io_state & H3_IO_RX_FIN) == 0) && (rs == SSL_STREAM_STATE_OK);
        const bool tx_open = ((sc->io_state & H3_IO_TX_FIN) == 0) && (ws == SSL_STREAM_STATE_OK);

        /* If *either* side is still open, the stream keeps the conn alive. */
        if (rx_open || tx_open) {
            return NS_TRUE;

        } else {
            /* Both sides are no longer OK: allow the conn to be freed only when the
               app/peer closure was observed and there is no buffered I/O left. */
            const bool queues_empty =
                (sc->tx_queued.unread == 0 &&
                 sc->tx_pending.unread == 0);
            SharedSnapshot snap = SharedSnapshotInit(&sc->sh);
            /* Closure observed when both halves have finished OR app closed explicitly. */
            const bool closure_observed =
                ((sc->io_state & (H3_IO_RX_FIN | H3_IO_TX_FIN)) == (H3_IO_RX_FIN | H3_IO_TX_FIN))
                || snap.closed_by_app;

            /* If closure happened and nothing is pending, it no longer keeps the conn alive. */
            return !(queues_empty && closure_observed);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_stream_map_empty --
 *
 *      Determine whether a QUIC connection has any active request streams
 *      remaining.  Iterates over all StreamCtx entries in the connection’s
 *      stream hash table and checks if any still "keep the connection alive"
 *      (i.e., are not fully closed or cleaned up).
 *
 * Results:
 *      Returns NS_TRUE if no live request streams remain (or if cc is NULL),
 *      otherwise NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
quic_conn_stream_map_empty(ConnCtx *cc)
{
    if (cc == NULL) {
        return NS_TRUE;
    } else if (cc->streams.numEntries == 0) { /* Tcl_HashTable field */
        /* Fast path: empty hash table */
        return NS_TRUE;
    } else {
        Tcl_HashSearch search;
        Tcl_HashEntry *e = Tcl_FirstHashEntry(&cc->streams, &search);

        while (e) {
            StreamCtx *sc = (StreamCtx *)Tcl_GetHashValue(e);

            if (sc != NULL && quic_stream_keeps_conn_alive(sc)) {
                return NS_FALSE; /* found a live request stream */
            }
            e = Tcl_NextHashEntry(&search);
        }
        return NS_TRUE;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_can_be_freed_postloop --
 *
 *      Determine whether a QUIC connection can be safely freed after
 *      the event loop completes. The function checks that the TLS
 *      handshake has finished, all HTTP/3 streams are closed, and both
 *      sides have completed a clean shutdown (SSL_SENT_SHUTDOWN and
 *      SSL_RECEIVED_SHUTDOWN).
 *
 * Results:
 *      Returns NS_TRUE if the connection is fully shut down and has no
 *      open streams; otherwise returns NS_FALSE.
 *
 * Side effects:
 *      Logs a diagnostic message when a connection becomes eligible for
 *      cleanup.
 *
 *----------------------------------------------------------------------
 */
static bool quic_conn_can_be_freed_postloop(SSL *conn, ConnCtx *cc)
{
    if (!SSL_is_init_finished(conn)) {
        /* Never free pre-handshake */
        return NS_FALSE;

    }  else if (cc->streams.numEntries != 0) {
        /* If any stream contexts exist, stay alive */
        return NS_FALSE;
    } else {
        int sd = SSL_get_shutdown(conn);
        bool both_shutdown    = (sd & SSL_SENT_SHUTDOWN) && (sd & SSL_RECEIVED_SHUTDOWN);
        bool no_open_streams  = quic_conn_stream_map_empty(cc);

        if (both_shutdown && no_open_streams) {
            Ns_Log(Notice, "H3 quic_conn_can_be_freed_postloop conn %p sd=%x entries=%d init=%d",
                   (void*)conn, sd, cc->streams.numEntries, SSL_is_init_finished(conn));
        }
        return both_shutdown && no_open_streams;
    }
}



/* QUIC Utilities */
/*
 *----------------------------------------------------------------------
 *
 * quic_udp_set_rcvbuf --
 *
 *      Configure the receive buffer size (SO_RCVBUF) for a UDP socket
 *      used by the QUIC listener or transport thread. This helps tune
 *      performance for high-throughput or high-concurrency QUIC traffic.
 *
 *      The function sets the kernel receive buffer size to the requested
 *      value and then queries it back via getsockopt() to report the
 *      effective value (some platforms may double or clamp it).
 *
 * Arguments:
 *      fd            - File descriptor of the UDP socket.
 *      rcvbuf_bytes  - Desired SO_RCVBUF size in bytes; if 0, the kernel
 *                      default is left unchanged.
 * Results:
 *      None.
 *
 * Side effects:
 *      May adjust the kernel receive buffer size for the socket.
 *      Logs both the requested and actual buffer sizes for diagnostics.
 *
 *----------------------------------------------------------------------
 */
static void
quic_udp_set_rcvbuf(int fd, size_t rcvbuf_bytes)
{
    /* rcvbuf_bytes == 0 means: leave kernel default */
    if (rcvbuf_bytes > 0) {
        size_t    size = rcvbuf_bytes, got = 0;
        socklen_t glen = (socklen_t)sizeof(got);

        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, (socklen_t)sizeof(size)) != 0) {
            Ns_Log(Warning, "udp(fd=%d): setsockopt(SO_RCVBUF=%ld) failed: %s",
                   fd, size, strerror(errno));
        }
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &glen) == 0) {
            Ns_Log(Notice, "udp(fd=%d): SO_RCVBUF requested=%ld, actual=%ld",
                   fd, size, got);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * quic_varint_len / quic_varint_decode --
 *
 *      Helpers for decoding QUIC’s variable-length integer format,
 *      as defined in RFC 9000 §16.  QUIC encodes integers in 1, 2, 4,
 *      or 8 bytes, with the length indicated by the top two bits of the
 *      first byte:
 *
 *          00xxxxxx – 1 byte
 *          01xxxxxx – 2 bytes
 *          10xxxxxx – 4 bytes
 *          11xxxxxx – 8 bytes
 *
 *      quic_varint_len() determines the encoded length from the first byte.
 *      quic_varint_decode() parses and reconstructs the full integer value
 *      from the given byte buffer, returning (uint64_t)-1 if incomplete.
 *
 * Arguments:
 *      b0 - First byte of the encoded integer (for quic_varint_len).
 *      p  - Pointer to the encoded byte sequence (for quic_varint_decode).
 *      n  - Number of bytes available in the buffer.
 *
 * Results:
 *      quic_varint_len() returns the number of bytes required to represent
 *      the value.
 *      quic_varint_decode() returns the decoded integer, or (uint64_t)-1
 *      if the buffer does not contain enough bytes.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static size_t quic_varint_len(uint8_t b0) {
    switch (b0 >> 6) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    default: return 8;
    }
}

static uint64_t quic_varint_decode(const uint8_t *p, size_t n) {
    size_t l = quic_varint_len(p[0]);
    uint64_t v = 0;

    if (l > n) return (uint64_t)-1;  // incomplete
    if (l == 1) return p[0] & 0x3f;
    if (l == 2) return ((uint64_t)(p[0] & 0x3f) << 8) | p[1];
    if (l == 4) return ((uint64_t)(p[0] & 0x3f) << 24) |
                    ((uint64_t)p[1] << 16) |
                    ((uint64_t)p[2] << 8) |
                    ((uint64_t)p[3]);
    // l == 8
    v  = ((uint64_t)(p[0] & 0x3f) << 56);
    v |= ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
        | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
    return v;
}

/*
 *----------------------------------------------------------------------
 *
 * quic_sid_to_stream --
 *
 *      Resolve a QUIC stream ID to its associated SSL stream object
 *      within the given connection context. This function maps both
 *      known unidirectional control streams (control, QPACK encoder/
 *      decoder) and dynamically created bidirectional request streams.
 *
 *      The lookup first checks the cached well-known stream IDs stored
 *      in the connection context, then falls back to querying the
 *      StreamCtx table for dynamically allocated streams.
 *
 * Arguments:
 *      cc  - Connection context containing QUIC stream mappings.
 *      sid - Stream ID to look up.
 *
 * Results:
 *      Returns the corresponding SSL* stream object if found,
 *      or NULL if the stream does not exist or has been freed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static SSL *quic_sid_to_stream(ConnCtx *cc, uint64_t sid)
{
    SSL *stream =
        sid == cc->h3ssl.cstream_id ? cc->h3ssl.cstream :
        sid == cc->h3ssl.pstream_id ? cc->h3ssl.pstream :
        sid == cc->h3ssl.rstream_id ? cc->h3ssl.rstream :
        sid == cc->client_qpack_enc_sid ? cc->client_qpack_enc_ssl :
        sid == cc->client_qpack_dec_sid ? cc->client_qpack_dec_ssl :
        NULL;
    if (stream == NULL) {
        StreamCtx *sc = StreamCtxGet(cc, (int64_t)sid, 0);

        stream = sc != NULL ? sc->ssl : NULL;
    }
    return stream;
}

/* QUIC Event handling & dispatch */
/*
 *----------------------------------------------------------------------
 *
 * quic_conn_handle_ic --
 *
 *      Accept and initialize incoming QUIC connections on a listening
 *      socket. This function is invoked by the UDP driver thread when
 *      OpenSSL’s QUIC server detects new connection attempts. For each
 *      accepted connection, it performs the following steps:
 *
 *        1. Accepts the QUIC connection via SSL_accept_connection().
 *        2. Creates a new ConnCtx object to represent the connection and
 *           associates it with both the Ns_Sock and SSL connection.
 *        3. Initializes a new nghttp3 server instance and default
 *           HTTP/3 settings for the connection.
 *        4. Configures OpenSSL to accept all incoming QUIC streams
 *           (control and request streams).
 *        5. Registers the connection in the pollset for event-driven I/O.
 *        6. Starts the QUIC handshake via SSL_do_handshake().
 *
 *      If any step fails (e.g., allocation, handshake, or nghttp3 setup),
 *      the function cleans up the connection context and frees the SSL
 *      handle.
 *
 * Arguments:
 *      listener_ssl - The OpenSSL QUIC listener handle that accepted
 *                     the new connection.
 *      drvPtr       - Pointer to the UDP driver, used to retrieve
 *                     the TLS configuration and socket interface.
 *
 * Results:
 *      None.  New connections are registered into the pollset on success.
 *
 * Side effects:
 *      May allocate a new ConnCtx, create associated nghttp3 state,
 *      modify pollset state, and log detailed diagnostic output.
 *
 *----------------------------------------------------------------------
 */
static void quic_conn_handle_ic(SSL *listener_ssl, Driver *drvPtr) {
    NsTLSConfig *dc = drvPtr->arg;

    for (;;) {
        nghttp3_settings settings;
        Ns_Time          now;
        Ns_Sock         *sockPtr;
        ConnCtx         *cc;
        int              ss, ret;
        SSL             *conn = SSL_accept_connection(listener_ssl, 0);
        char             buffer[NS_IPADDR_SIZE];

        Ns_Log(Notice, "[%lld] H3 quic_conn_handle_ic gets conn %p from listener_ssl %p",
               (long long)dc->iter, (void*)conn, (void*) listener_ssl);

        if (conn == NULL) {
            // no more pending connections
            break;
        }

        //OSSL_TRY(SSL_set_msg_callback(conn, ossl_msg_cb));

        SSL_set_app_data(conn, dc);

        Ns_GetTime(&now);
        ss = NsSockAccept((Ns_Driver*)drvPtr, SSL_get_fd(listener_ssl), (Ns_Sock**)&sockPtr, &now, conn);

        (void)ns_inet_ntop((const struct sockaddr *)&sockPtr->sa, buffer, NS_IPADDR_SIZE);
        Ns_Log(Notice, "[%lld] H3 CONN accept SockAccept returns sock state %d, sockPtr %p IP %s",
               (long long)dc->iter, ss, (void*)sockPtr, buffer);

        assert(drvPtr == ((Sock*)sockPtr)->drvPtr);

        /* 2) Create ConnCtx and bind it both to the Ns_Sock and the SSL */
        cc = ConnCtxNew(dc, conn);
        if (cc == NULL) {
            Ns_Log(Error, "could no allocate H3 ConnCtx");
            NsSockClose((Sock*)sockPtr, /*keep=*/0);
            SSL_free(conn);
            break;
        }

        Ns_Log(Notice, "[%lld] H3 SockAccept can associate sock %p with cc %p",
               (long long)dc->iter, (void*)sockPtr, (void*)cc);
        SSL_set_ex_data(conn, dc->u.h3.cc_idx, cc);

        /* 3) Initialize nghttp3 server on that new connection */
        nghttp3_settings_default(&settings);
        settings.max_field_section_size = 16 * 1024;  // 16KB
        //settings.qpack_max_dtable_capacity = 4096;
        //settings.qpack_blocked_streams = 100;

        Ns_Log(Notice, "[%lld] H3 quic_conn_handle_ic settings"
               " qpack_max_dtable_capacity %lu"
               " qpack_blocked_streams %lu"
               " max_field_section_size %llu",
               (long long)dc->iter,
               settings.qpack_max_dtable_capacity,
               settings.qpack_blocked_streams,
               (long long)settings.max_field_section_size);

        if (nghttp3_conn_server_new(&cc->h3conn, &h3_callbacks,
                                    &settings, nghttp3_mem_default(),
                                    cc) != 0) {
            Ns_Log(Error, "could not create H3 nghttp3 server connection");
            ns_free(cc);
            SSL_free(conn);
            continue;
        }

        cc->client_max_bidi_streams = 100; // initial max number of client bidi streams
        nghttp3_conn_set_max_client_streams_bidi(cc->h3conn, cc->client_max_bidi_streams);

        /*
         * Tell OpenSSL to accept *all* incoming QUIC streams
         */
        OSSL_TRY(SSL_set_incoming_stream_policy(conn,
                                                SSL_INCOMING_STREAM_POLICY_ACCEPT,
                                                0 /* application error code on reject */));

        /* 4) Finally, add it into active-connection list so Recv/Send see it */
        PollsetAddConnection(dc, conn,
                             SSL_POLL_EVENT_OSB | SSL_POLL_EVENT_OSU |
                             SSL_POLL_EVENT_ISB | SSL_POLL_EVENT_ISU);

        Ns_Log(Notice, "[%lld] H3 accept_connection cc->h3ssl.conn %p ex_data %p",
               (long long)dc->iter, (void*)cc->h3ssl.conn,
               dc?(void*)SSL_get_ex_data(cc->h3ssl.conn, dc->u.h3.cc_idx):0);
        //log_local_cert(dc, conn, "in quic_conn_handle_ic");

        // After creating ConnCtx and nghttp3_conn_server_new():
        OSSL_TRY(SSL_set_accept_state(conn));  // Explicitly set server mode

        // Set handshake to manual mode
        OSSL_TRY(SSL_set_mode(conn, SSL_MODE_AUTO_RETRY));

        // Start handshake immediately
        ret = SSL_do_handshake(conn);
        Ns_Log(Notice, "H3 quic_conn_handle_ic conn %p SSL_do_handshake -> %d", (void*)conn, ret);
        if (ret <= 0) {
            int err = SSL_get_error(conn, ret);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                // Immediate failure
                ossl_log_error_detail(err, "quic_conn_handle_ic");
                PollsetMarkDead(cc, conn, "IC handshake failed");
            }
        }

        // if we break here, we get accept a single connection
        break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * quic_conn_handle_e --
 *
 *      Handle QUIC connection-level error and shutdown events reported
 *      by the OpenSSL QUIC pollset. This function interprets event flags
 *      such as EC (error condition), ECD (definitive connection death),
 *      and ER/EW (read/write error hints) to decide whether the connection
 *      should be torn down or remain active.
 *
 *      On EC or ECD, the connection is considered irrecoverable and is
 *      shut down immediately via quic_conn_enter_shutdown().
 *      On ER/EW, the function first drives any pending internal state
 *      transitions with SSL_handle_events(), then checks whether the
 *      connection can safely be freed using quic_conn_can_be_freed().
 *
 * Arguments:
 *      cc      - Connection context for the QUIC session.
 *      conn    - OpenSSL QUIC connection object.
 *      revents - Pollset event mask indicating which error events occurred.
 *
 * Results:
 *      Returns NS_TRUE if the connection is definitively closed and should
 *      not be processed further; NS_FALSE otherwise.
 *
 * Side effects:
 *      May trigger connection shutdown and free associated streams or
 *      state if fatal events are detected. Logs all significant state
 *      transitions and OpenSSL error details.
 *
 *----------------------------------------------------------------------
 */
static bool quic_conn_handle_e(ConnCtx *cc, SSL *conn, uint64_t revents)
{
    NsTLSConfig *dc = cc->dc;

    if (revents & SSL_POLL_EVENT_EC) {
        unsigned long err = ERR_peek_error();
        if (err) {
            char buf[120];
            ERR_error_string_n(err, buf, sizeof(buf));
            Ns_Log(Error, "[%lld] EC QUIC connection error: %s", (long long)dc->iter, buf);
        }
        /* Treat EC as fatal for our pollset: tear down once */
        quic_conn_enter_shutdown(cc, "EC");
        return NS_TRUE; /* caller should skip further handling */
    }

    if (revents & SSL_POLL_EVENT_ECD) {
        /* Definitive connection death */
        quic_conn_enter_shutdown(cc, "ECD");
        return NS_TRUE;
    }

    if (revents & (SSL_POLL_EVENT_ER | SSL_POLL_EVENT_EW)) {
        /* Drive timers/state; then decide if we can/should tear down */
        SSL_handle_events(conn);
        Ns_Log(Notice, "[%lld] SSL_handle_events in quic_conn_handle_e conn %p => %d",
               (long long)dc->iter, (void*)conn, SSL_handle_events(conn));


        /* If OpenSSL reports the connection closed, our heuristic says so, close. */
        if (quic_conn_can_be_freed(conn, revents, cc)) {
            quic_conn_enter_shutdown(cc, "ER/EW->closed");
            return NS_TRUE;
        }
        /* Otherwise, keep the conn item; ER/EW alone aren't fatal. */
    }

    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * quic_stream_handle_e --
 *
 *      Handle stream-level error and shutdown events for an HTTP/3 stream
 *      running over QUIC. This function interprets OpenSSL pollset error
 *      flags (ER, EW) for individual streams, performs cleanup of closed
 *      read/write sides, and updates polling interest accordingly.
 *
 *      When the read side signals an error (ER), the function attempts to
 *      drain pending data via h3_stream_drain() - which internally calls
 *      SSL_read_ex() and forwards received frames to nghttp3. If the stream
 *      reaches EOF or a fatal error, it is marked dead in the pollset.
 *
 *      When the write side is closed or encounters EW, the stream’s write
 *      interest is disarmed to avoid redundant polling. If both read and
 *      write sides are no longer active, the stream is marked dead entirely.
 *
 * Arguments:
 *      cc            - Connection context owning the stream.
 *      stream        - OpenSSL QUIC stream object.
 *      sid           - QUIC stream ID.
 *      revents       - Pollset event mask containing error bits.
 *      current_mask  - Current active pollset mask for this stream.
 *
 * Results:
 *      Returns NS_TRUE if the stream has been fully removed (no longer active),
 *      or NS_FALSE if it remains valid but with reduced polling interest.
 *
 * Side effects:
 *      May update pollset event masks, call nghttp3 drain handlers,
 *      mark streams as dead, and emit detailed diagnostic logs.
 *
 *----------------------------------------------------------------------
 */

static bool quic_stream_handle_e(ConnCtx *cc, SSL *stream, uint64_t sid,
                                 uint64_t revents, uint64_t current_mask)
{
    NsTLSConfig *dc = cc->dc;
    StreamCtx   *sc = SSL_get_ex_data(stream, dc->u.h3.sc_idx);
    bool         removed = NS_FALSE;

    /* Read-side exception: try to drain once. Treat ER similar to R. */
    if (revents & SSL_POLL_EVENT_ER) {
        H3DrainResultCode dr = h3_stream_drain(cc, stream, sid, "handle E flags");   /* does SSL_read_ex(), feeds nghttp3 */

        if (dr == DRAIN_EOF || dr == DRAIN_ERROR) {
            PollsetMarkDead(cc, stream, "stream ER");
            return NS_TRUE;                /* caller should skip further handling */
        }
    }

    /* Write-side exception or closed write side: stop polling for W. */
    if ((revents & SSL_POLL_EVENT_EW) || SSL_get_stream_write_state(stream) != SSL_STREAM_STATE_OK) {
        if (current_mask & SSL_POLL_EVENT_W) {
            (void)PollsetUpdateEvents(dc, stream, sc, /*set=*/0, /*clear=*/SSL_POLL_EVENT_W);
        }
        /* Not removed; just disarmed W */
    }

    /* Optional: if both sides are closed, drop the stream even if drain was idle. */
    {
        int rs = SSL_get_stream_read_state(stream);
        int ws = SSL_get_stream_write_state(stream);
        if (rs != SSL_STREAM_STATE_OK && ws != SSL_STREAM_STATE_OK) {
            Ns_Log(Notice, "[%lld] H3[%llu] ER/EW both sides are closed rs=%s ws=%s io=%u seen_io=%u kind=%s",
                   (long long)dc->iter, (long long)sid,
                   ossl_quic_stream_state_str(rs), ossl_quic_stream_state_str(ws),
                   (unsigned)(sc ? sc->io_state : 0),
                   (unsigned)(sc ? sc->seen_io  : 0),
                   (sc ? H3StreamKind_str(sc->kind) : "no-ctx"));
            PollsetMarkDead(cc, stream, "stream ER|EW, both sides closed");
            return NS_TRUE;
        }
    }

    /* Optional trace for diagnostics */
    if (revents & (SSL_POLL_EVENT_ER | SSL_POLL_EVENT_EW)) {
        int rs = SSL_get_stream_read_state(stream);
        int ws = SSL_get_stream_write_state(stream);
        Ns_Log(Notice, "[%lld] H3[%llu] ER/EW handled: rs=%s ws=%s io=%u seen_io=%u kind=%s",
               (long long)dc->iter, (long long)sid,
               ossl_quic_stream_state_str(rs), ossl_quic_stream_state_str(ws),
               (unsigned)(sc ? sc->io_state : 0),
               (unsigned)(sc ? sc->seen_io  : 0),
               (sc ? H3StreamKind_str(sc->kind) : "no-ctx"));
    }

    return removed;
}


/*
 *----------------------------------------------------------------------
 *
 * quic_stream_handle_r --
 *
 *      Handle read-side (R) events for a single HTTP/3 QUIC stream.
 *      This function drains incoming data from the OpenSSL QUIC stream,
 *      feeds it into nghttp3 for header/body processing, updates the
 *      per-stream state machine, and decides whether the stream remains
 *      active or should be torn down.
 *
 *      The function:
 *        * Validates stream ID and context.
 *        * Calls h3_stream_drain() to consume available data and update
 *          nghttp3.
 *        * Reacts to EOF or reset events by marking the stream closed,
 *          unregistering it, or disarming read polling.
 *        * For client-initiated bidirectional streams (H3_KIND_BIDI_REQ),
 *          dispatches finished requests to the application when the
 *          receive side becomes ready (H3_IO_REQ_READY).
 *        * Triggers write readiness when a read produces progress or when
 *          response data might be generated.
 *        * Frees the stream entirely when both halves are finished or reset.
 *
 * Arguments:
 *      cc      - Connection context that owns the stream.
 *      stream  - OpenSSL QUIC stream object ready for reading.
 *
 * Results:
 *      Returns NS_TRUE if the stream has been fully closed and removed from
 *      the pollset; NS_FALSE if it remains active.
 *
 * Side effects:
 *      May perform I/O through SSL_read_ex(), update nghttp3 state,
 *      disable polling on completed halves, unregister stream contexts,
 *      and log detailed read-path state transitions.
 *
 *----------------------------------------------------------------------
 */
static bool
quic_stream_handle_r(ConnCtx *cc, SSL *stream)
{
    NsTLSConfig *dc = cc->dc;
    int64_t      sid = (int64_t)SSL_get_stream_id(stream);

    /* Resolve StreamCtx (defensive  -  keeps semantics identical to inline block) */
    StreamCtx *sc = SSL_get_ex_data(stream, dc->u.h3.sc_idx);

    if (sid < 0) {
        Ns_Log(Error, "[%lld] H3[?] R: invalid stream id; resetting",
               (long long)dc->iter);
        PollsetMarkDead(cc, stream, "invalid sid on read");
        return NS_TRUE; /* slot is dead */
    }

    if (sc == NULL) {
        int st = SSL_get_stream_type(stream); /* defensive info only */

        Ns_Log(Error, "[%lld] H3[%lld] R: missing StreamCtx (type=%d); resetting stream",
               (long long)dc->iter, (long long)sid, st);

        /* Remove from pollset and free the orphaned SSL* */
        PollsetMarkDead(cc, stream, "missing sc on read");
        return NS_TRUE; /* slot is dead */
    }
    NS_TA_ASSERT_HELD(cc, affinity);

    sc->seen_readable = NS_TRUE;

    /* Drain readable data into hold/buffers and drive nghttp3 */
    {
        H3DrainResultCode dr = h3_stream_drain(cc, stream, (uint64_t)sid, "processing R");

        Ns_Log(Notice, "[%lld] H3[%lld] R h3_stream_drain %p -> %d (%s)",
               (long long)dc->iter, (long long)sid, (void*)stream, dr, H3DrainResultCode_str(dr));

        (void)SSL_handle_events(stream);

        Ns_Log(Notice, "[%lld] H3[%lld] R h3_stream_drain kind %s leads to io_state %.2x",
               (long long)dc->iter, (long long)sid, H3StreamKind_str(sc->kind), sc->io_state);

        /* If a client BIDI request became ready, dispatch it now. */
        if (sc->kind == H3_KIND_BIDI_REQ
            && (sc->io_state & H3_IO_REQ_READY)
            && !(sc->io_state & H3_IO_REQ_DISPATCHED)) {

            Ns_Log(Notice, "[%lld] H3[%lld] SSL_handle_events in poll event R -> DISPATCH",
                   (long long)dc->iter, (long long)sid);

            if (SockDispatchFinishedRequest(sc) == NS_OK) {
                sc->io_state &= (uint8_t)~H3_IO_REQ_READY;
            } else {
                Ns_Log(Warning, "[%lld] H3[%lld] dispatch failed",
                       (long long)dc->iter, (long long)sid);
            }
        }

        switch (dr) {
        case DRAIN_PROGRESS:
            /* Read progressed; writer might have control frames/body to send */
            cc->wants_write = NS_TRUE;
            sc->seen_io     = NS_TRUE;
            break;

        case DRAIN_EOF:    /* peer sent FIN on read half */
        case DRAIN_CLOSED: /* treat as read-half closed for our state machine */
            sc->io_state |= H3_IO_RX_FIN;

            if (sc->kind == H3_KIND_BIDI_REQ) {
                /* Keep BIDI alive for TX; stop polling R on this stream */
                PollsetDisableRead(dc, stream, sc, "event R, EOF|closed");
                cc->wants_write = NS_TRUE;  /* response headers/data may be ready */
                (void)h3_stream_maybe_finalize(sc, "R: EOF");
            } else {
                /* Client UNI: safe to tear down immediately */
                StreamCtxUnregister(sc);
                PollsetMarkDead(cc, stream, "uni read complete");
                return NS_TRUE; /* slot is dead */
            }
            break;

        case DRAIN_ERROR:
            sc->io_state |= H3_IO_RESET;
            StreamCtxUnregister(sc);
            PollsetMarkDead(cc, stream, "stream error");
            return NS_TRUE; /* slot is dead */

        case DRAIN_NONE:
        default:
            break;
        }
    }

    /* Finalize if both halves done or reset */
    if (h3_stream_can_free(sc)) {
        PollsetMarkDead(cc, stream, "stream both halves done");
        return NS_TRUE; /* slot is dead */
    }

    Ns_Log(Notice, "[%lld] H3[%lld] R post-drain io_state %.2x",
           (long long)dc->iter, (long long)sid, sc->io_state);

    return NS_FALSE; /* keep slot */
}

/*
 *----------------------------------------------------------------------
 *
 * DStringAppendSslPollEventFlags --
 *
 *      Produce a human-readable, '|'-separated representation of an
 *      SSL_POLL_EVENT_* bitmask and append it to the specified Tcl_DString.
 *      This helper is primarily used for diagnostic logging of QUIC/SSL
 *      pollset event masks.
 *
 *      Each known event bit (e.g., SSL_POLL_EVENT_R, SSL_POLL_EVENT_W,
 *      SSL_POLL_EVENT_EC, etc.) is mapped to a short label such as
 *      "R", "W", or "EC". If no bits are set, the string "NONE" is appended.
 *
 * Results:
 *      Returns the internal C string pointer (dsPtr->string) after appending
 *      the decoded flag labels.
 *
 * Side effects:
 *      Extends the Tcl_DString with the formatted flag list.
 *      Intended for logging and debugging only - not for parsing.
 *
 *----------------------------------------------------------------------
 */
static char *
DStringAppendSslPollEventFlags(Tcl_DString *dsPtr, uint64_t flags)
{
    int count = 0;
    /* map each SSL_POLL_EVENT_* bit to its label */
    static const struct {
        uint64_t flag;
        const char *label;
    } options[] = {
        { SSL_POLL_EVENT_F,   "F"   },
        { SSL_POLL_EVENT_EL,  "EL"  },
        { SSL_POLL_EVENT_EC,  "EC"  },
        { SSL_POLL_EVENT_ECD, "ECD" },
        { SSL_POLL_EVENT_ER,  "ER"  },
        { SSL_POLL_EVENT_EW,  "EW"  },
        { SSL_POLL_EVENT_R,   "R"   },
        { SSL_POLL_EVENT_W,   "W"   },
        { SSL_POLL_EVENT_IC,  "IC"  },
        { SSL_POLL_EVENT_ISB, "ISB" },
        { SSL_POLL_EVENT_ISU, "ISU" },
        { SSL_POLL_EVENT_OSB, "OSB" },
        { SSL_POLL_EVENT_OSU, "OSU" },
    };

    NS_NONNULL_ASSERT(dsPtr != NULL);

    for (size_t i = 0; i < sizeof(options)/sizeof(options[0]); i++) {
        if (flags & options[i].flag) {
            if (count > 0) {
                Tcl_DStringAppend(dsPtr, "|", 1);
            }
            Tcl_DStringAppend(dsPtr, options[i].label, TCL_INDEX_NONE);
            count++;
        }
    }

    if (count == 0) {
        /* no bits set */
        Tcl_DStringAppend(dsPtr, "NONE", TCL_INDEX_NONE);
    }

    return dsPtr->string;
}

/*======================================================================
 * Function Implementations: HTTP/3 connection-level scheduling
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * h3_conn_write_step --
 *
 *      Drive one HTTP/3 (nghttp3) -> QUIC (OpenSSL) transmit pass for a
 *      single connection. This function:
 *        - Drains per-stream "resume" notifications and (re)submits headers
 *          or resumes streams in nghttp3 as needed.
 *        - Pulls the next writable stream and payload vectors from
 *          nghttp3 via nghttp3_conn_writev_stream().
 *        - Maps the nghttp3 stream ID to its SSL* and writes each vector
 *          with SSL_write_ex2(), handling SSL_ERROR_WANT_* and protocol
 *          exceptions (STOP_SENDING, send-only, shutdown, etc.).
 *        - Marks offsets as consumed in nghttp3, concludes streams (FIN)
 *          when appropriate, and advances per-stream state (io_state).
 *        - Enables/disables POLLOUT per stream based on pending data or
 *          WANT_* conditions, and calls SSL_handle_events() to flush
 *          per-stream and connection-level timers/acks/coalesced frames.
 *        - Reaps streams which are finished or reset, and may mark the
 *          connection as closing on protocol shutdown conditions.
 *
 * Results:
 *      Returns NS_TRUE if the writer should be scheduled again soon
 *      (because at least one stream kept POLLOUT armed or any stream
 *      returned SSL_ERROR_WANT_*). Returns NS_FALSE if nothing remains
 *      immediately actionable (no WANT_* and no stream asked to keep W).
 *
 * Side effects:
 *      - Modifies nghttp3 connection/stream state (offsets, FIN, shutdown).
 *      - Updates StreamCtx fields (io_state, seen_io, wants_write, etc.).
 *      - Enables/disables POLLOUT on individual streams in the pollset.
 *      - May call PollsetMarkDead() to remove finished/reset streams.
 *      - May set cc->connection_state to closing on protocol shutdown.
 *      - Calls SSL_handle_events() on streams and the connection to drive
 *        QUIC packetization, timers, and acknowledgements.
 *
 * Notes:
 *      - Skips work if the connection is already entering shutdown or if
 *        SSL_get_shutdown(conn) is non-zero.
 *      - Handles zero-length FINs produced by nghttp3 (no payload vectors).
 *      - Uses a conservative policy: on WRITE errors (other than WANT_*),
 *        logs details and schedules another pass or tears down as needed.
 *
 *----------------------------------------------------------------------
 */
static bool
h3_conn_write_step(ConnCtx *cc)
{
    nghttp3_vec   vecs[WRITE_STEP_MAX_VEC];
    nghttp3_ssize nvec;
    int64_t       sid = -1;
    int           fin = 0;
    bool          did_progress = NS_FALSE;   /* any bytes written or FIN concluded */
    bool          any_keep_w   = NS_FALSE;  /* kept W armed on at least one stream */
    bool          hit_any_want = NS_FALSE;  /* saw SSL_ERROR_WANT_* on any stream */
    NsTLSConfig  *dc = cc->dc;

    Ns_Log(Notice, "[%lld] H3 h3_conn_write_step called", (long long)dc->iter);

    /* Don’t write when we’re closing/closed at our layer */
    if (cc->connection_state != 0) {
        Ns_Log(Notice, "[%lld] H3 write: cc closing; skip", (long long)dc->iter);
        return NS_FALSE;
    }

    {
        Tcl_HashSearch  search;
        Tcl_HashEntry  *e;

        for (e = Tcl_FirstHashEntry(&cc->streams, &search);
             e != NULL;
             e = Tcl_NextHashEntry(&search)) {
            StreamCtx *sc = Tcl_GetHashValue(e);

            if (sc != NULL && StreamCtxIsBidi(sc)) {
                /* clear for all streams */
                Ns_Log(Notice, "[%lld] H3[%lld] h3_conn_write_step: clear tx_served_this_step",
                       (long long)dc->iter, (long long)sc->quic_sid);
                sc->tx_served_this_step = NS_FALSE;
                //sc->rx_emitted_in_pass = 0;
            }
        }
    }


    {
        int64_t sids[64];
        size_t  nres;

        /* Drain "resume" ring and poke nghttp3 */
        nres = SharedDrainResume(&cc->shared, sids, 64);
        Ns_Log(Notice, "[%lld] H3 drain-resume count=%zu", (long long)cc->dc->iter, nres);

        for (size_t i = 0; i < nres; ++i) {
            const int64_t sid = sids[i];
            SSL *s            = quic_sid_to_stream(cc, (uint64_t)sids[i]);
            StreamCtx *ssc    = s
                ? SSL_get_ex_data(s, cc->dc->u.h3.sc_idx)
                : StreamCtxGet(cc, sid, /*create*/0);

            if (ssc == NULL || !StreamCtxIsBidi(ssc)) {
                Ns_Log(Notice, "[%lld] H3[%lld] has no BIDI stream context",
                       (long long)cc->dc->iter, (long long)sids[i]);
                continue;
            }

            /* Clear the per-stream resume flag once per drain */
            //SharedResumeClear(&ssc->sh);

            /*
             * If headers became ready, submit them now.
             */
            if (!ssc->hdrs_submitted && SharedHdrsIsReady(&ssc->sh)) {
                if (h3_stream_submit_ready_headers(ssc) != 0) {
                    /* error already logged inside; continue to next sid */
                    continue;
                }
            }

            /*
             * If this stream uses a data reader (body or zero-length FIN
             * after headers), poke nghttp3 so it will call the read
             * callback.
             */
            if (ssc->hdrs_submitted) {
                (void)nghttp3_conn_resume_stream(cc->h3conn, sid);
                /* Ensure we get a POLLOUT tick to push frames */
                if (ssc->ssl != NULL) {
                    PollsetEnableWrite(cc->dc, ssc->ssl, ssc, "resume");
                }
            }
            SharedResumeClear(&ssc->sh);

            Ns_Log(Notice, "[%lld] H3[%lld] resume", (long long)cc->dc->iter, (long long)sids[i]);
        }

        if (/*did_submit ||*/ nres > 0) {
            Ns_Log(Notice, "[%lld] H3 drive conn after resume via SSL_handle_events", (long long)cc->dc->iter);
            //SSL_handle_events(cc->h3ssl.conn);
            //Ns_Log(Notice, "[%lld] H3 drive conn after resume via SSL_handle_events DONE", (long long)cc->dc->iter);
        }
    }

    /* Don’t start writes if QUIC conn already in TLS shutdown */
    ERR_clear_error();
    if (SSL_get_shutdown(cc->h3ssl.conn) != 0) {
        Ns_Log(Notice, "[%lld] H3 write: conn already in shutdown; skip", (long long)dc->iter);
        return NS_FALSE;
    }

    for (;;) {
        bool       hit_want   = NS_FALSE;
        bool       finalized  = NS_FALSE;
        SSL       *stream     = NULL;
        StreamCtx *sc         = NULL;

        sid = -1;
        nvec = nghttp3_conn_writev_stream(cc->h3conn, &sid, &fin, vecs, WRITE_STEP_MAX_VEC);

        Ns_Log(Notice, "[%lld] H3[%lld] writev: rv=%ld %s fin=%d",
               (long long)dc->iter, (long long)sid, (long)nvec,
               nvec > 0 ? "OK" : nvec == 0 ? "NOTHING" : nghttp3_strerror((int)nvec),
               fin);

        for (int i=0; i<nvec; i++) {
            Ns_Log(Notice, "[%lld] H3[%lld] ... vec[%d] len %ld",
                   (long long)dc->iter, (long long)sid, i, vecs[i].len);
        }

        if (nvec <= 0) {
            if (sid >= 0 && fin) {

                // Zero-length FIN for a stream (often one we already freed at TLS level)
                Ns_Log(Notice, "[%lld] H3[%lld] writev: ZERO-LEN FIN; calling nghttp3_conn_shutdown_stream_write",
                       (long long)dc->iter, (long long)sid);

                /* Tell nghttp3 the app is done writing on this stream. Harmless if repeated. */
                nghttp3_conn_shutdown_stream_write(cc->h3conn, sid);

                { /* zero-length FIN (no payload vectors left) */
                    StreamCtx *zsc = StreamCtxGet(cc, sid, /*create*/0);
                    if (zsc  != NULL) {
                        int ok = SSL_stream_conclude(zsc->ssl, 0);

                        Ns_Log(Notice, "[%lld] H3[%lld] writev: rv=%ld %s fin=%d -> SSL_stream_conclude -> %d",
                               (long long)dc->iter, (long long)sid, (long)nvec,
                               nvec > 0 ? "OK" : nvec == 0 ? "NOTHING" : nghttp3_strerror((int)nvec),
                               fin, ok);

                        if (ok == 1) {
                            uint8_t io_state;
                            /* Mark TX closed under lock; remember we sent FIN. */

                            Ns_MutexLock(&zsc->lock);
                            zsc->io_state |= H3_IO_TX_FIN;
                            zsc->eof_sent  = NS_TRUE;
                            io_state = zsc->io_state;
                            Ns_MutexUnlock(&zsc->lock);

                            /* SSL_stream_conclude() only queues a QUIC STREAM FIN; it may not produce an
                             * immediate per-stream kernel event to wake the poll loop. With a long poll
                             * timeout this can delay the final service pass (and reaping). We therefore set
                             * sc->wants_write = NS_TRUE and PollsetEnableWrite() as a one-shot nudge so the
                             * writer promptly observes TX_FIN and empty queues and either PollsetMarkDead()
                             * if RX is finished, or disables W and leaves only R armed. This trims tail
                             * latency without spinning; a future connection-level "kick" or shorter timeout
                             * could make this nudge unnecessary.
                             */
                            zsc->wants_write = NS_TRUE;
                            PollsetEnableWrite(dc, zsc->ssl, sc, "tx-fin");
                            {
                                /* If RX is already finished and queues are empty, reap now. */
                                SharedSnapshot snap = SharedSnapshotInit(&zsc->sh);
                                bool rx_done = (io_state & H3_IO_RX_FIN) || zsc->eof_seen;
                                bool tx_done = (io_state & H3_IO_TX_FIN) && SharedIsEmpty(&snap);

                                if (rx_done && tx_done) {
                                    PollsetMarkDead(cc, zsc->ssl, "finalize both-done");
                                }
                            }
                        } else {
                            int err = SSL_get_error(zsc->ssl, ok);
                            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                                zsc->wants_write = NS_TRUE;
                                PollsetEnableWrite(dc, zsc->ssl, zsc, "tx fin WANT_*");
                            } else {
                                /* Don’t crash the stream; mark reset and drop W. */
                                Ns_MutexLock(&zsc->lock);
                                zsc->io_state |= H3_IO_RESET;
                                Ns_MutexUnlock(&zsc->lock);
                                PollsetDisableWrite(dc, zsc->ssl, zsc, "tx fin fail->reset");
                            }
                        }
                    }
                }

                /*
                 * Kick the QUIC engine once at the *connection* to enqueue/flush FIN/ACKs.
                 * We keep per-stream W armed above so the next poll cycle can finish flushing.
                 */
                (void)SSL_handle_events(cc->h3ssl.conn);
                did_progress = NS_TRUE;

                // IMPORTANT: don’t break; try next stream this tick, this might be a leftover of a former request
                continue;
            }

            if (nvec == 0) {
                h3_conn_clear_wants_write_if_idle(cc);
            }
            break;
        }

        stream = quic_sid_to_stream(cc, (uint64_t)sid);
        if (stream == NULL) {
            /*
             * No stream for this sid
             */
            h3_stream_skip_write_and_trim(cc, StreamCtxGet(cc, sid, 0), sid, vecs, (int)nvec, fin,
                                          "no SSL* for stream");
            did_progress = NS_TRUE;
            continue;
        }

        sc = SSL_get_ex_data(stream, dc->u.h3.sc_idx);

        //Ns_Log(Notice, "[%lld] H3 write map: nghttp3 sid=%lld -> ssl %p (sid=%llu) kind=%s",
        //       (long long)dc->iter, (long long)sid, (void*)stream, (uint64_t)SSL_get_stream_id(stream),
        //       sc ? H3StreamKind_str(sc->kind) : "no-ctx");

        /* Re-check connection shutdown just before IO */
        if (SSL_get_shutdown(cc->h3ssl.conn) != 0) {
            Ns_Log(Notice, "[%lld] H3 write: conn entered shutdown pre-write; stop", (long long)dc->iter);
            return NS_FALSE;
        }

        /* Respect per-stream write state */
        if (SSL_get_stream_write_state(stream) != SSL_STREAM_STATE_OK) {
            H3DiscardState ds;

            Ns_Log(Notice, "[%lld] H3[%lld] skip write: ws=%d kind=%s",
                   (long long)dc->iter, (long long)sid, SSL_get_stream_write_state(stream),
                   sc ? H3StreamKind_str(sc->kind) : "no-ctx");

            ds = h3_stream_skip_write_and_trim(cc, sc, h3_stream_id(sc), vecs, (int)nvec, fin, "stream state not OK");
            if (ds & (H3_DISCARD_ADVANCED | H3_DISCARD_FIN)) {
                did_progress = NS_TRUE;
                if (sc != NULL) {
                    sc->seen_io = NS_TRUE;
                }
            }

            /* Drive the stream object once to clear readiness */
            (void)SSL_handle_events(stream);
            Ns_Log(Notice, "[%lld] SSL_handle_events in h3_conn_write_step stream %p => %d",
                   (long long)dc->iter, (void*)stream, SSL_handle_events(stream));

            /* If write-half is closed, don’t keep W armed */
            PollsetDisableWrite(dc, stream, sc, "h3_conn_write_step SSL_STREAM_STATE not OK");
            goto after_sid;
        }

        /* Write each vec fully (or bail on WANT_*); add offset once per vec */
        for (int i = 0; i < (int)nvec; ++i) {
            size_t off = 0;

            while (off < vecs[i].len) {
                size_t    written = 0;
                uint64_t  flags   = 0;
                int       ok;

                if (sc->kind == H3_KIND_BIDI_REQ && i == 0) {
                    const uint8_t *bufPtr = vecs[i].base + off;
                    size_t         varint1_len = quic_varint_len(bufPtr[0]);
                    uint64_t       varint1     = quic_varint_decode(bufPtr, vecs[i].len  - off);
                    size_t         varint2_len = quic_varint_len(bufPtr[varint1_len]);
                    uint64_t       varint2     = quic_varint_decode(bufPtr+varint1_len, vecs[i].len - (off + varint1_len));

                    Ns_Log(Notice, "[%lld] H3[%lld] SANITY CHECK"
                           " varint 1: len %ld value %" PRIu64
                           " varint 2: len %ld value %" PRIu64,
                           (long long)dc->iter, (long long)sc->quic_sid,
                           varint1_len, varint1,
                           varint2_len, varint2);
                }

                Ns_Log(Notice, "[%lld] H3[%lld] want to write %ld bytes on %s writable %d"
                       " blocking stream %d conn %d",
                       (long long)dc->iter, (long long)sid, vecs[i].len, H3StreamKind_str(sc->kind), sc->writable,
                       SSL_get_blocking_mode(stream), SSL_get_blocking_mode(cc->h3ssl.conn));

                ok = SSL_write_ex2(stream,
                                   vecs[i].base + off,
                                   vecs[i].len  - off,
                                   flags,
                                   &written);

                Ns_Log(Notice, "[%lld] H3[%lld] SSL_write_ex2 stream %p len %ld flags %04" PRIx64 ": ok %d written %ld",
                       (long long)dc->iter, (long long)sid, (void*)stream, vecs[i].len  - off, flags, ok, written);

                if (ok != 1) {
                    int err = SSL_get_error(stream, ok);

                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        hit_want = NS_TRUE;
                        /* No offsets advanced for partial vec: retry next poll */
                        (void)SSL_handle_events(stream);
                        Ns_Log(Notice, "[%lld] SSL_handle_events in h3_conn_write_step WANT stream %p",
                               (long long)dc->iter, (void*)stream);

                        goto after_sid;                        /* don’t advance remaining vecs */
                    }

                    if (err == SSL_ERROR_SSL) {
                        unsigned long e = ERR_peek_error();
                        if (ERR_GET_LIB(e) == ERR_LIB_SSL) {
                            int r = ERR_GET_REASON(e);

                            if (r == SSL_R_STREAM_RESET) {
                                uint64_t appw = 0;

                                (void)SSL_handle_events(stream);
                                //(void)SSL_handle_events(cc->h3ssl.conn);

                                if (SSL_get_stream_write_error_code(stream, &appw) == 1) {
                                    Ns_Log(Notice, "[%lld] H3[%lld] peer STOP_SENDING app=0x%llx",
                                           (long long)dc->iter, (long long)sid, (unsigned long long)appw);
                                } else {
                                    /* Some OpenSSL versions only expose it on the *read* side, or it isn't latched yet */
                                    uint64_t appr = 0;
                                    if (SSL_get_stream_read_error_code(stream, &appr) == 1) {
                                        Ns_Log(Notice, "[%lld] H3[%lld] peer app error (read side) app=0x%llx",
                                               (long long)dc->iter, (long long)sid, (unsigned long long)appr);
                                    } else {
                                        Ns_Log(Notice, "[%lld] H3[%lld] peer reset: no app code available yet",
                                               (long long)dc->iter, (long long)sid);
                                    }
                                }

                                /* Don’t advance remaining offsets on this vec. */
                                nghttp3_conn_shutdown_stream_write(cc->h3conn, sid);
                                SharedMarkClosedByApp(&sc->sh);
                                ERR_clear_error();
                                (void)SSL_handle_events(stream);
                                Ns_Log(Notice, "[%lld] SSL_handle_events in h3_conn_write_step ERR stream %p",
                                       (long long)dc->iter, (void*)stream);

                                PollsetDisableWrite(dc, stream, sc, "h3_conn_write_step SSL_R_STREAM_RESET");
                                goto next_sid;
                            }

                            if (r == SSL_R_STREAM_SEND_ONLY) {
                                /* Treat this vec as skipped; advance to keep nghttp3 moving. */
                                Ns_Log(Notice, "[%lld] H3[%lld] send-only restriction; skip vec",
                                       (long long)dc->iter, (long long)sid);
                                h3_stream_advance_and_trim(sc, sid, vecs[i].base, vecs[i].len);
                                did_progress = NS_TRUE;
                                break; /* next vec */
                            }

                            if (r == SSL_R_PROTOCOL_IS_SHUTDOWN) {
                                /* Connection-level teardown - propagate and stop writing. */
                                Ns_Log(Notice, "[%lld] H3[%lld] protocol is shutdown; marking conn closing",
                                       (long long)dc->iter, (long long)sid);
                                cc->connection_state = 1;
                                ERR_clear_error();
                                return NS_TRUE; /* writer should be scheduled to complete shutdown */
                            }

                            Ns_Log(Error, "[%lld] H3[%lld] SSL_write_ex2: reason=%d (%s)",
                                   (long long)dc->iter, (long long)sid, r, ERR_reason_error_string(e));
                            /* Don’t advance offsets for the partial vec; let nghttp3 retry/adjust. */
                            return NS_TRUE; /* be conservative: schedule another pass */
                        }
                    }

                    /* Non-SSL error path: log and stop; keep offsets unchanged for retry. */
                    ossl_log_error_detail(err, "h3_conn_write_step");
                    return NS_TRUE;
                }

                /*
                 * Chunk written successfully.
                 */
                //NsHexPrint("write buffer", vecs[i].base, vecs[i].len, 32, NS_FALSE);

                off           += written;
                did_progress   = NS_TRUE;
                sc->seen_io    = NS_TRUE;
            }

            /* Vec fully written -> now tell nghttp3 it’s consumed */
            h3_stream_advance_and_trim(sc, sid, vecs[i].base, vecs[i].len);
        }

        /* Attach FIN after all vecs (only for bidi data streams). */
        if (fin && StreamCtxIsBidi(sc) && !H3_IO_HAS(sc, H3_IO_TX_FIN)) {
            int ok;

            /* Stop nghttp3 from generating more body for this stream either way. */
            nghttp3_conn_shutdown_stream_write(cc->h3conn, h3_stream_id(sc));

            /* Only attempt conclude if the write side is still OK. */
            if (SSL_get_stream_write_state(stream) == SSL_STREAM_STATE_OK) {
                /* Prefer explicit conclude over WRITE_FLAG_CONCLUDE on data writes. */
                ok = SSL_stream_conclude(stream, 0);   /* or SSL_stream_conclude(stream) if our API has no flags */
                if (ok == 1) {
                    sc->io_state |= H3_IO_TX_FIN;
                    did_progress  = NS_TRUE;

                    Ns_Log(Notice, "[%lld] H3 write_step conclude sets sc->wants_write", (long long)dc->iter);
                    sc->wants_write = NS_TRUE; /* one shot */
                } else {
                    const int err = SSL_get_error(stream, ok);
                    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                        /* Don’t set TX_FIN yet; keep EW armed so conclude can complete next tick. */
                        /* h3_stream_maybe_finalize() will try again when drained. */
                        Ns_Log(Notice, "[%lld] H3 write_step WANT sets sc->wants_write", (long long)dc->iter);
                        sc->wants_write = NS_TRUE; /* one shot */
                    } else {
                        /* Hard failure on conclude: treat as terminal on write side to avoid loops. */
                        sc->io_state |= H3_IO_RESET;
                        PollsetDisableWrite(dc, stream, sc, "h3_conn_write_step Hard failure on conclude");
                    }
                }
            } else {
                /* Write side already not-OK: don’t try to conclude again. */
            }
        }

        /* Finalize once per SID, after conclude attempt. */
        finalized = StreamCtxIsServerUni(sc) ? NS_FALSE : h3_stream_maybe_finalize(sc, "h3_conn_write_step");
        if (!finalized) {
            (void)SSL_handle_events(stream);
            //Ns_Log(Notice, "[%lld] SSL_handle_events in h3_conn_write_step after_sid stream %p",
            //       (long long)dc->iter, (void*)stream);
        }

    after_sid:
        /*
         * IMPORTANT: drive the STREAM once per SID batch to clear its W/R readiness,
         * schedule datagrams, and process acks/timeouts related to this stream.
         */
        (void)SSL_handle_events(stream);
        //Ns_Log(Notice, "[%lld] SSL_handle_events in h3_conn_write_step after_sid stream %p => %d",
        //       (long long)dc->iter, (void*)stream, SSL_handle_events(stream));

        /* Per-stream W decision:
         * - keep W if we hit WANT_* on this stream,
         * - or if app still has tx pending for this stream.
         */
        if (StreamCtxIsServerUni(sc)) {
            /* leave policy as-is for CTRL/QPACK */
        } else if (hit_want || SharedTxReadable(&sc->sh)) {
            PollsetEnableWrite(dc, stream, sc, "after_sid");
            any_keep_w = NS_TRUE;
        } else {
            PollsetDisableWrite(dc, stream, sc, "h3_conn_write_step per stream W decision");
        }

        if (hit_want) {
            hit_any_want = NS_TRUE;
        }
    next_sid:
        /* Continue outer loop to pull next sid/vecs from nghttp3 */
        ;
    }


    /* If nghttp3 reported a zero-length FIN (no vecs), ghttp3 wants a FIN without payload */
    if (nvec == 0 && sid >= 0 && fin) {
        SSL *stream = quic_sid_to_stream(cc, (uint64_t)sid);

        if (stream != NULL) {
            StreamCtx *sc = SSL_get_ex_data(stream, dc->u.h3.sc_idx);

            /* Only for bidi data streams; skip control/uni */
            if (sc != NULL && StreamCtxIsBidi(sc) && !(sc->io_state & H3_IO_TX_FIN)) {
                /* If stream write state is OK, conclude; ignore if already done */
                if (SSL_get_stream_write_state(stream) == SSL_STREAM_STATE_OK) {
                    (void)SSL_stream_conclude(stream, 0);
                }
                nghttp3_conn_shutdown_stream_write(cc->h3conn, sid);
                sc->io_state |= H3_IO_TX_FIN;

                /* Drive the stream once to clear readiness and schedule frames */
                (void)SSL_handle_events(stream);
                Ns_Log(Notice, "[%lld] SSL_handle_events in h3_conn_write_step FIN stream %p",
                       (long long)dc->iter, (void*)stream);

                did_progress = NS_TRUE;
                PollsetDisableWrite(dc, stream, sc, "h3_conn_write_step zero-length FIN");
            }
        }
    }

    /*
     * Drive the CONNECTION once if anything happened (bytes written, FIN).
     * This flushes coalesced frames across streams into datagrams.
     */
    if (did_progress || hit_any_want || any_keep_w) {
        (void)SSL_handle_events(cc->h3ssl.conn);
        Ns_Log(Notice, "[%lld] SSL_handle_events in h3_conn_write_step final conn %p",
               (long long)dc->iter, (void*)cc->h3ssl.conn);
    }

    /*
     * Decide "still wants": keep scheduling if any stream kept W,
     * or we hit WANT_* on any stream.
     */
    return (any_keep_w || hit_any_want) ? NS_TRUE : NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_conn_clear_wants_write_if_idle --
 *
 *      Clear the connection-level "wants_write" flag if there is no
 *      remaining pending work (no active streams or queued writes).
 *      This prevents unnecessary POLLOUT wakeups once the HTTP/3
 *      connection is fully idle.
 *
 *      The function checks via h3_conn_has_work() whether any streams
 *      still require writing. If none do, it resets cc->wants_write to
 *      NS_FALSE and calls PollsetUpdateConnPollInterest() to drop the
 *      connection’s write interest from the pollset.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - May clear cc->wants_write.
 *      - Updates pollset interest flags for the connection.
 *      - Emits diagnostic log messages indicating whether work remains.
 *
 *----------------------------------------------------------------------
 */
static void
h3_conn_clear_wants_write_if_idle(ConnCtx *cc) {
    bool has_work = h3_conn_has_work(cc);

    NS_TA_ASSERT_HELD(cc, affinity);


    Ns_Log(Notice, "[%lld] H3 conn: h3_conn_clear_wants_write_if_idle has work %d",
           (long long)cc->dc->iter, has_work);

    if (!has_work) {
        if (cc->wants_write) {
            Ns_Log(Notice, "[%lld] H3 conn: idle now, clearing wants_write", (long long)cc->dc->iter);
        }
        cc->wants_write = NS_FALSE;
        PollsetUpdateConnPollInterest(cc);   /* drops conn-level EW */
    }
}

/*
 *----------------------------------------------------------------------
 *
 * h3_conn_has_work --
 *
 *      Determine whether a given HTTP/3 connection (ConnCtx) still has
 *      pending work that requires writer activity. This includes both
 *      connection-level and per-stream conditions that imply outgoing
 *      data, frames, or control actions are still pending.
 *
 *      Specifically, the function returns true if any of the following
 *      hold:
 *        - The connection itself has cc->wants_write set.
 *        - There are streams pending in the shared resume queue
 *          (e.g., ready for header or body submission).
 *        - Any stream still has pending headers, queued body data, or
 *          flagged wants_write=true.
 *
 * Results:
 *      Returns NS_TRUE if the connection or any of its streams still
 *      have pending output or resumable work; otherwise NS_FALSE.
 *
 * Side effects:
 *      None. This is a pure inspection routine, but is frequently used
 *      to decide whether to clear cc->wants_write or to keep POLLOUT
 *      armed for the connection.
 *
 *----------------------------------------------------------------------
 */
static bool
h3_conn_has_work(ConnCtx *cc) {
    Tcl_HashSearch it;
    Tcl_HashEntry *he;

    /* Connection-wide "something to push soon" */
    if (cc->wants_write) {
        return NS_TRUE;
    }

    /* Any streams enqueued for resume by producers? */
    if (SharedHasResumePending(&cc->shared)) {
        return NS_TRUE;
    }

    for (he = Tcl_FirstHashEntry(&cc->streams, &it); he != NULL; he = Tcl_NextHashEntry(&it)) {
        StreamCtx *sc = (StreamCtx *)Tcl_GetHashValue(he);
        if (sc == NULL) {
            continue;
        } else {
            SharedSnapshot snap = SharedSnapshotInit(&sc->sh);

            if (sc->wants_write)                 return NS_TRUE;
            if (StreamCtxIsBidi(sc)
                && SharedHdrsIsReady(&sc->sh)
                && !sc->hdrs_submitted)             return NS_TRUE;            /* headers ready to submit */

            /* body present now or staged for next pull */
            if (SharedHasData(&snap)) return NS_TRUE;
        }
    }
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_conn_mark_wants_write --
 *
 *      Mark the HTTP/3 connection as needing a write pass, and ensure
 *      that the specified stream is also POLLOUT-enabled. This helper
 *      is typically called when new data, headers, or FIN frames become
 *      ready to send, or when a previous SSL_write_ex2() returned a
 *      WANT_* condition that requires rescheduling.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Sets cc->wants_write = NS_TRUE, ensuring the writer thread will
 *        perform another h3_conn_write_step().
 *      - Calls PollsetEnableWrite() on the given stream if it still has
 *        an active SSL object, so it will be serviced on the next pass.
 *
 *----------------------------------------------------------------------
 */
static inline void
h3_conn_mark_wants_write(ConnCtx *cc, StreamCtx *sc, const char *why)
{
    NS_TA_ASSERT_HELD(cc, affinity);

    cc->wants_write = NS_TRUE;  /* drives another h3_conn_write_step pass */

    /* Also re-arm W on the specific stream if we still have it */
    if (sc->ssl) {
        PollsetEnableWrite(cc->dc, sc->ssl, sc, why ? why : "wants_write");
    }
}

/*
 *----------------------------------------------------------------------
 *
 * h3_conn_maybe_raise_client_bidi_credit --
 *
 *      Adjust the QUIC/HTTP3 connection’s advertised limit for the
 *      number of concurrent client-initiated bidirectional streams if
 *      needed. Each incoming stream ID encodes its ordinal number as
 *      (sid >> 2), and the server must increase its "max streams bidi"
 *      credit when it observes a higher ordinal than previously allowed.
 *
 *      This helper computes the desired cumulative credit (ordinal + 1)
 *      and updates both the nghttp3 connection state and the cached
 *      cc->client_max_bidi_streams value.
 *
 * Arguments:
 *      cc   - Connection context.
 *      sid  - QUIC stream ID of a newly seen client-initiated BIDI stream.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Calls nghttp3_conn_set_max_client_streams_bidi() when credit
 *        must be raised.
 *      - Updates cc->client_max_bidi_streams.
 *
 *----------------------------------------------------------------------
 */
static inline void
h3_conn_maybe_raise_client_bidi_credit(ConnCtx *cc, uint64_t sid)
{
    /* client-initiated bidi ordinals: 0,1,2,…  => sid >> 2 */
    uint64_t ord1 = (sid >> 2) + 1;       /* desired cumulative credit */

    if (ord1 > cc->client_max_bidi_streams) {
        nghttp3_conn_set_max_client_streams_bidi(cc->h3conn, ord1);
        cc->client_max_bidi_streams = ord1;
        Ns_Log(Notice, "h3 bidi credit -> %llu", (unsigned long long)ord1);
    }
}

/*======================================================================
 * Function Implementations: HTTP/3 Header Processing
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_build_resp_headers --
 *
 *      HTTP/3 response header encoder used by the NaviServer driver to
 *      translate a merged Ns_Set of response headers into a contiguous
 *      array of nghttp3_nv name/value pairs stored within the StreamCtx.
 *
 *      This function is registered as an Ns_HeaderEncodeFn callback and
 *      is invoked by Ns_FinalizeResponseHeaders(). It performs the H3-
 *      specific mapping from HTTP/1.x-style headers to HPACK-like name/
 *      value pairs without emitting any textual CRLF output.
 *
 *      Steps performed:
 *        1. Resets the stream’s header store.
 *        2. Inserts a mandatory ":status" pseudo-header (remapping 101 -> 200).
 *        3. Appends all regular headers from the merged set, excluding
 *           hop-by-hop fields invalid in HTTP/3.
 *        4. Finalizes nghttp3_nv pointers to contiguous storage.
 *        5. Publishes the header array and metadata into the StreamCtx.
 *
 * Results:
 *      Returns NS_TRUE on success, NS_FALSE on error.
 *      Sets *out_len (if provided) to the number of nghttp3_nv entries.
 *
 * Side effects:
 *      - Allocates and populates sc->resp_nv[] and sc->resp_nv_store.
 *      - Frees any previously allocated header arrays.
 *      - Logs each header for diagnostic tracing.
 *      - Updates StreamCtx flags for response body eligibility and
 *        content-length presence.
 *
 *----------------------------------------------------------------------
 */
static bool
h3_stream_build_resp_headers(Ns_Conn *conn,
                             const Ns_Set *merged,
                             void *UNUSED(out_obj),
                             size_t *out_len)
{
    NsTLSConfig *dc;
    StreamCtx   *sc;
    size_t       nvlen = 0, nvcap = 0;
    nghttp3_nv  *nva = NULL;
    Ns_Sock     *sock = Ns_ConnSockPtr(conn);
    Tcl_DString *store;
    int          status = ((Conn *)conn)->responseStatus;
    bool         success = NS_TRUE; /* we want to push an empty iovec in successful cases */

    if (sock == NULL) {
        goto fail;
    }

    dc = sock->driver->arg;
    sc = StreamCtxFromSock(dc, sock);
    if (sc == NULL) {
        goto fail;
    }

    store = &sc->resp_nv_store;

    /* Start fresh for this response */
    Tcl_DStringSetLength(store, 0);
    if (sc->resp_nv) {
        ns_free(sc->resp_nv);
        sc->resp_nv = NULL;
    }
    sc->resp_nvlen = 0;

    /* 1) :status  -  map 101 -> 200 for HTTP/3 */
    {
        char s3[3];

        if (status == 101) {
            Ns_Log(Notice, "h3: status code 101 not allowed in HTTP/3; remapping to 200");
            status = 200;
        }
        s3[0] = (char)('0' + (status / 100) % 10);
        s3[1] = (char)('0' + (status / 10)  % 10);
        s3[2] = (char)('0' + (status % 10));
        if (h3_headers_nv_append(store, &nva, &nvlen, &nvcap, ":status", 7, s3, 3) != 0) {
            goto fail;
        }
    }

    /* 2) Regular headers from merged set.
     *    Keys are guaranteed lower-case already.
     *    Filter hop-by-hop headers which are illegal/meaningless in H3.
     */
    for (size_t i = 0; i < Ns_SetSize(merged); ++i) {
        size_t klen, vlen;
        const char *key = Ns_SetKey(merged, i);
        const char *val = Ns_SetValue(merged, i);

        if (key == NULL || val == NULL) {
            continue;
        }

        klen = strlen(key);
        vlen = strlen(val);

        /* Drop hop-by-hop */
        if (h3_headers_is_invalid_response_field(key, klen, val, vlen)) {
            continue;
        }
        //sanitize_value_h3(val, &vbuf);

        if (h3_headers_nv_append(store, &nva, &nvlen, &nvcap,
                                 key, klen,
                                 val, vlen) != 0) {
            Ns_Log(Error, "h3: could not push response header to nghttp3");
            goto fail;
        }
    }

    /* 3) Finalize pointers into the contiguous store.
     *    IMPORTANT: do not append to store after this point.
     */
    {
        const uint8_t *ptr = (const uint8_t *)store->string;
        for (size_t i = 0; i < nvlen; ++i) {
            nva[i].name  = (const uint8_t *)ptr;
            ptr += nva[i].namelen;
            nva[i].value = (const uint8_t *)ptr;
            ptr += nva[i].valuelen;
        }
    }

    /* Publish to StreamCtx */
    sc->resp_nv    = nva;
    sc->resp_nvlen = nvlen;

    h3_headers_log_nv(sc, sc->resp_nv,  sc->resp_nvlen, "h3_stream_build_resp_headers");

    sc->response_allow_body = h3_response_allows_body(status, sc->method);
    {
        const char *contentLength = Ns_SetGetValue(merged, "content-length", NULL);

        sc->response_has_non_zero_content_length = contentLength != NULL
            ? strtol(contentLength, NULL, 10) > 0
            : NS_FALSE;
    }

 done:
    if (out_len != NULL) {
        *out_len = nvlen;
    }

    /* No CRLF bytes produced  -  let the writer submit nv to nghttp3. */
    return success;

 fail:
    if (nva != NULL) {
        ns_free(nva);
    }
    nvlen = 0;
    success = NS_FALSE;
    goto done;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_headers_is_invalid_response_field --
 *
 *      Determine whether a given HTTP header field is invalid or
 *      prohibited in an HTTP/3 *response*. This routine is used to
 *      filter out headers that violate HTTP/3 or RFC 9114 rules before
 *      encoding or forwarding a response header block.
 *
 *      Specifically, the following are considered invalid:
 *        - Pseudo-header fields (names beginning with ':').
 *        - Empty field names.
 *        - HTTP/1.x hop-by-hop headers such as "Connection",
 *          "Keep-Alive", "Proxy-Connection", "Upgrade",
 *          and "transfer-encoding".
 *        - The "TE" header, which is only valid in requests.
 *
 * Results:
 *      Returns NS_TRUE if the header must be excluded from an HTTP/3
 *      response, NS_FALSE otherwise.
 *
 * Side effects:
 *      None. This is a pure predicate used for response header filtering.
 *
 *----------------------------------------------------------------------
 */
static bool
h3_headers_is_invalid_response_field(const char *name, size_t nlen,
                                     const char *val,  size_t vlen)
{
    /* Defensive: never forward pseudo or empty */
    if (nlen == 0 || name[0] == ':') {
        return NS_TRUE;
    }

    /* HTTP/1.1 hop-by-hop headers are forbidden in H2/H3 */
    if ((nlen == 10 && memcmp(name, "connection",         10) == 0) ||
        (nlen == 10 && memcmp(name, "keep-alive",         10) == 0) ||
        (nlen == 17 && memcmp(name, "proxy-connection",   17) == 0) ||
        (nlen == 7  && memcmp(name, "upgrade",            7)  == 0) ||
        (nlen == 17 && memcmp(name, "transfer-encoding",  17) == 0)) {
        return NS_TRUE;
    }

    /*
     * The "TE" (transfer-encoding) header field is only meaningful/allowed
     * for requests and not for responses; just drop it.
     */
    if (nlen == 2 && name[0] == 't' && name[1] == 'e') {
        (void)val; (void)vlen;
        return NS_TRUE;
    }

    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_headers_field_section_size --
 *
 *      Compute the total encoded size of an HTTP/3 (QPACK) header field
 *      section according to RFC 9114 §4.1.1, which defines the field
 *      section size as the sum of each field’s name length, value length,
 *      and an additional 32 bytes of overhead per field.
 *
 * Results:
 *      Returns the computed total field section size in bytes.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static size_t
h3_headers_field_section_size(const nghttp3_nv *nva, size_t nvlen)
{
    size_t n = 0;
    for (size_t i = 0; i < nvlen; ++i) {
        n += nva[i].namelen + nva[i].valuelen + 32; /* per RFC semantics */
    }
    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_headers_nv_append --
 *
 *      Append a single HTTP/3 (nghttp3) name/value header pair to a
 *      growing dynamic array of nghttp3_nv structures and to a backing
 *      Tcl_DString buffer that holds the serialized header bytes.
 *
 *      This function handles both capacity growth and safe accumulation:
 *        - If the nv array is full, it reallocates it with doubled capacity.
 *        - The header name and value bytes are appended back-to-back to
 *          the Tcl_DString; only lengths are stored in nghttp3_nv.
 *        - No pointers into Tcl_DString are stored during construction,
 *          since its memory may move as it grows.
 *        - The caller can later fix up the final base pointers once
 *          the DString’s address becomes stable.
 *
 * Results:
 *      Returns 0 on success, or NGHTTP3_ERR_NOMEM if allocation fails.
 *
 * Side effects:
 *      - May reallocate *pnva.
 *      - Extends the Tcl_DString with header name/value bytes.
 *      - Updates *pnvlen and *pnvcap.
 *
 *----------------------------------------------------------------------
 */
static int
h3_headers_nv_append(Tcl_DString *store, nghttp3_nv **pnva, size_t *pnvlen, size_t *pnvcap,
                     const char *name, size_t nlen, const char *val, size_t vlen)
{
    nghttp3_nv *nva = *pnva;
    size_t      nvlen = *pnvlen, nvcap = *pnvcap;

    if (nvlen == nvcap) {
        size_t newcap = (nvcap == 0 ? 8 : nvcap * 2);
        nghttp3_nv *nv2 = (nghttp3_nv *)ns_realloc(nva == NULL ? NULL : nva, newcap * sizeof(*nva));

        //Ns_Log(Notice, "h3_headers_nv_append performs REALLOC");
        if (nv2 == NULL) {
            return NGHTTP3_ERR_NOMEM;
        }
        /* if previously on stack, caller passed pnva pointing to heap already;
           here we always use heap to keep C99 simple. */
        *pnva   = nva = nv2;
        *pnvcap = nvcap = newcap;
    }

    /*
     * Append name and value to the backing string buffer.
     *
     * We intentionally do NOT take pointers here. Tcl_DString may reallocate as it
     * grows, which would invalidate any pointers captured during construction.
     *
     * Instead we record only lengths (namelen/valuelen). Because we append each
     * header as two back-to-back segments  -  [name][value], with no separators  -  the
     * bytes in the DString are densely packed and in a deterministic order. We’ll
     * compute the final pointers in a single sweep after the last append, when the
     * buffer address is stable.
     *
     * NOTE: This relies on the invariant that for every header we append `name`
     * immediately followed by `value`, and that no other bytes are inserted between
     * headers during construction.
     */

    Tcl_DStringAppend(store, name, (TCL_SIZE_T)nlen);
    Tcl_DStringAppend(store, val,  (TCL_SIZE_T)vlen);

    nva[nvlen].namelen  = nlen;
    nva[nvlen].valuelen = vlen;
    nva[nvlen].flags    = NGHTTP3_NV_FLAG_NONE;

    *pnvlen = nvlen + 1;

    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_headers_log_nv --
 *
 *      Produce a human-readable log of a set of HTTP/3 (nghttp3) header
 *      name/value pairs for diagnostic or debugging purposes.
 *
 *      The function formats all headers in "name: value" form, separated
 *      by newlines, into a temporary Tcl_DString, then emits a single
 *      Ns_Log(Notice) message including:
 *        - the stream ID and connection iteration counter,
 *        - a descriptive label (e.g., "request", "response"),
 *        - the number of fields,
 *        - the estimated total field section size (RFC 9114 semantics),
 *        - the peer’s advertised maximum field section size, and
 *        - the formatted headers themselves.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Logs a formatted header dump at Notice level; allocates a temporary
 *      Tcl_DString which is freed before returning.
 *
 *----------------------------------------------------------------------
 */
static void
h3_headers_log_nv(const StreamCtx *sc, const nghttp3_nv *nva, size_t nvlen, const char* label)
{
    Tcl_DString ds;

    Tcl_DStringInit(&ds);
    for (size_t i = 0; i < nvlen; i++) {
        Tcl_DStringAppend(&ds,  (const char*)nva[i].name, (TCL_SIZE_T)nva[i].namelen);
        Tcl_DStringAppend(&ds, ": ", 2);
        Tcl_DStringAppend(&ds, (const char*)nva[i].value,  (TCL_SIZE_T)nva[i].valuelen);
        Tcl_DStringAppend(&ds, "\n", 1);
    }

    /*Ns_Log(Notice, "H3[%lld] nva section_size=%zu (peer_max_field_section_size=%llu, nv=%zu) header storage %d bytes in %p",
      (long long)sc->h3_sid, h3_headers_field_section_size(nva, nvlen),
      cc->peer_max_field_section_size, nvlen,
      sc->resp_nv_store.length, (void*)sc);*/

    Ns_Log(Notice, "[%lld] H3[%lld] NVA %s (%ld header fields, estimated size %ld, peer_max_size %" PRIu64 ")\n%s",
           (long long)sc->cc->dc->iter, (long long)sc->quic_sid, label, nvlen,
           h3_headers_field_section_size(nva, nvlen),
           sc->cc->client_max_field_section_size,
           ds.string);

    Tcl_DStringFree(&ds);
}


/* H3 data flow */
/*
 *----------------------------------------------------------------------
 *
 * h3_stream_feed_pending --
 *
 *      Feed any buffered receive data for a given HTTP/3 stream into the
 *      nghttp3 connection for decoding and header/body processing.
 *
 *      The function reads from the stream’s local receive buffer
 *      (sc->rx_hold, sc->rx_off … sc->rx_len) and calls
 *      nghttp3_conn_read_stream() repeatedly until all pending data is
 *      consumed or the peer flow control blocks further reading.
 *
 *      For client-initiated unidirectional (UNI) streams that are marked
 *      as ignorable, the function simply discards unread data without
 *      passing it to nghttp3.
 *
 *      When the stream’s receive buffer becomes empty and a deferred FIN
 *      (end of stream) flag is pending, a final zero-length read is issued
 *      with fin=1 to deliver the end-of-stream signal to nghttp3.
 *
 * Results:
 *      Returns an H3FeedResultCode:
 *          FEED_OK_PROGRESS - data consumed normally
 *          FEED_OK_BLOCKED  - nghttp3 temporarily blocked, retry later
 *          FEED_EOF         - stream FIN delivered
 *          FEED_ERR         - nghttp3 reported an error
 *
 * Side effects:
 *      - Advances sc->rx_off as data is consumed.
 *      - May clear sc->rx_fin_pending and set sc->eof_seen.
 *      - Calls nghttp3_conn_read_stream() one or more times.
 *      - Produces verbose diagnostic logs.
 *
 *----------------------------------------------------------------------
 */
static H3FeedResultCode
h3_stream_feed_pending(StreamCtx *sc, uint64_t sid)
{
    /* Drain-and-ignore payload for UNI streams we don't hand to nghttp3. */
    if (StreamCtxIsClientUni(sc) && sc->ignore_uni && sc->rx_off < sc->rx_len) {
        sc->rx_off = sc->rx_len;
        return FEED_OK_PROGRESS;
    }

    while (sc->rx_off < sc->rx_len) {
        const uint8_t *p = sc->rx_hold + sc->rx_off;
        size_t         n = sc->rx_len  - sc->rx_off, adv;
        ssize_t        rv;

        sc->rx_emitted_in_pass = 0;   // reset before each call of nghttp3_conn_read_stream

        Ns_Log(Notice, "[%lld] H3[%llu] h3_stream_feed_pending into nghttp3_conn_read_stream buffer %p len %ld (no fin)",
               (long long)sc->cc->dc->iter, (unsigned long long)sid, (const void*)p, n);
        rv = nghttp3_conn_read_stream(sc->cc->h3conn, (int64_t)sid, p, n, /*fin*/0);
        Ns_Log(Notice, "[%lld] H3[%llu] h3_stream_feed_pending into nghttp3_conn_read_stream buffer %p len %ld (no fin) -> consumed %ld recv %ld",
               (long long)sc->cc->dc->iter, (unsigned long long)sid, (const void*)p, n, rv, sc->rx_emitted_in_pass);

        if (rv < 0) return FEED_ERR;

        adv = (size_t)rv + sc->rx_emitted_in_pass;
        if (adv > n) {
            /* safety belt, should not be necessary */
            adv = n;
        }

        if (adv == 0) return FEED_OK_BLOCKED;

        sc->rx_off += adv;
    }

    /* Window fully consumed; if FIN was pending, deliver now. */
    if (sc->rx_off == sc->rx_len) {
        sc->rx_off = sc->rx_len = 0;
        if (sc->rx_fin_pending) {
            (void)nghttp3_conn_read_stream(sc->cc->h3conn, (int64_t)sid, NULL, 0, /*fin*/1);
            sc->rx_fin_pending = NS_FALSE;
            sc->eof_seen = NS_TRUE;
            return FEED_EOF; /* fine to report EOF-style progress */
        }
    }
    return FEED_OK_PROGRESS;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_read_into_hold --
 *
 *      Attempt to read additional QUIC stream data from OpenSSL into the
 *      stream’s receive staging buffer (sc->rx_hold), but only if that
 *      buffer is currently empty.
 *
 *      This function performs a non-blocking read using SSL_read_ex() and
 *      interprets the result according to QUIC and HTTP/3 semantics:
 *
 *        - If data is read successfully, it updates rx_len and returns
 *          DRAIN_PROGRESS.
 *        - If SSL indicates WANT_READ or WANT_WRITE, it returns DRAIN_NONE
 *          (the caller should retry later).
 *        - If SSL signals EOF (SSL_ERROR_ZERO_RETURN), it marks either an
 *          immediate or deferred FIN depending on whether data was already
 *          buffered, and returns DRAIN_EOF.
 *        - Any other error is logged via ossl_log_error_detail() and
 *          reported as DRAIN_ERROR.
 *
 * Results:
 *      Returns one of:
 *          DRAIN_PROGRESS - new bytes were read into rx_hold
 *          DRAIN_NONE     - no data available (WANT_* condition)
 *          DRAIN_EOF      - peer closed read side
 *          DRAIN_ERROR    - unrecoverable SSL or protocol error
 *
 * Side effects:
 *      - Advances internal rx_off/rx_len counters.
 *      - May set sc->rx_fin_pending or sc->eof_seen.
 *      - Calls nghttp3_conn_read_stream() if a FIN is delivered immediately.
 *      - Emits diagnostic logs about SSL and QUIC stream state.
 *
 *----------------------------------------------------------------------
 */
static H3DrainResultCode
h3_stream_read_into_hold(StreamCtx *sc, SSL *stream)
{
    ConnCtx *cc = sc->cc;
    size_t   nread;
    int      ok;

    if (sc->rx_len != sc->rx_off) {
        return DRAIN_PROGRESS; /* still have bytes to feed */
    }
    sc->rx_len = sc->rx_off = 0;

    nread = 0;
    ok = SSL_read_ex(stream, sc->rx_hold, sc->rx_cap, &nread);
    ossl_log_stream_and_conn_states(cc, stream, cc->h3ssl.conn, SSL_STREAM_STATE_OK, "drain after SSL_read");
    if (ok != 1) {
        int err = SSL_get_error(stream, ok);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return DRAIN_NONE;
        if (ok == 0 && err == SSL_ERROR_ZERO_RETURN) {
            /* EOF now: if nothing buffered, deliver FIN immediately; otherwise defer FIN */
            if (sc->rx_len == 0) {
                (void)nghttp3_conn_read_stream(cc->h3conn, (int64_t)sc->h3_sid, NULL, 0, 1);
                sc->eof_seen = NS_TRUE;
                return DRAIN_EOF;
            }
            sc->rx_fin_pending = NS_TRUE;
            return DRAIN_EOF;
        }
        ossl_log_error_detail(err, "h3_stream_drain");
        return DRAIN_ERROR;
    }
    if (nread == 0) return DRAIN_NONE;

    sc->rx_len = nread; /* rx_off stays 0 */
    return DRAIN_PROGRESS;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_advance_and_trim --
 *
 *      Advance nghttp3’s write offset for a stream after successfully
 *      transmitting `nbytes` of payload, and trim the corresponding data
 *      from the stream’s shared transmit buffer. This keeps nghttp3’s
 *      internal flow control state synchronized with the application’s
 *      queued output.
 *
 *      The function also checks whether the stream’s transmit queue has
 *      reached end-of-body (EOF) conditions: if all data has been sent and
 *      the application marked the stream as closed, nghttp3 is explicitly
 *      resumed and the stream is scheduled for a final FIN emission.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Advances nghttp3’s write offset via nghttp3_conn_add_write_offset().
 *      - Removes `nbytes` from the stream’s pending transmit buffer.
 *      - If EOF-ready, resumes the nghttp3 stream and schedules a write
 *        pass to emit the FIN.
 *      - Produces detailed diagnostic logs about trimming and flow state.
 *
 *----------------------------------------------------------------------
 */
static inline void
h3_stream_advance_and_trim(StreamCtx *sc, int64_t sid, uint8_t *base, size_t nbytes)
{

    Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_advance_and_trim ENTER bytes %ld",
           (long long)sc->cc->dc->iter, (long long)sc->quic_sid, nbytes);

    if (nbytes != 0) {
        ConnCtx       *cc = sc->cc;
        SharedSnapshot snap;
        size_t         body_trimmed;

        nghttp3_conn_add_write_offset(cc->h3conn, sid, nbytes);

        body_trimmed = SharedTrimPendingFromVec(&sc->sh, base, nbytes);
        if (body_trimmed) {
            Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_advance_and_trim TRIM body %zu (vec len %zu)",
                   (long long)cc->dc->iter, (long long)sc->quic_sid, body_trimmed, (size_t)nbytes);
        } else {
            Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_advance_and_trim SKIP trim (framing/headers) %zu",
                   (long long)cc->dc->iter, (long long)sc->quic_sid, (size_t)nbytes);
        }

        {
            SharedSnapshot snap0 = SharedSnapshotInit(&sc->sh);
            Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_advance_and_trim ENTER after trim queued %ld pending %ld closed:by_app=%d bytes %ld",
                   (long long)cc->dc->iter, (long long)sc->quic_sid, snap0.queued_bytes, snap0.pending_bytes, snap0.closed_by_app, nbytes);
        }
        SharedSnapshotRead(&sc->sh, &snap);

        if (SharedEOFReady(&snap)) {
            nghttp3_conn_resume_stream(cc->h3conn, sid);

            h3_conn_mark_wants_write(cc, sc, "emit FIN");
            PollsetEnableWrite(cc->dc, sc->ssl, sc, "drained->EOF");
            Ns_Log(Notice, "[%lld] H3[%lld] drained; scheduling EOF FIN",
                   (long long)cc->dc->iter, (long long)sc->h3_sid);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_skip_write_and_trim --
 *
 *      Advance nghttp3's write offset and optionally trim pending output
 *      data when a stream’s outgoing vectors are skipped or discarded
 *      (for example, due to a stream reset, send-only restriction, or
 *      protocol shutdown). This ensures that nghttp3’s flow-control
 *      accounting remains consistent even if no actual write occurs.
 *
 *      The function computes the total length of all provided nghttp3_vec
 *      entries, advances the corresponding write offset in nghttp3, and,
 *      if a StreamCtx is available, trims the same number of bytes from
 *      its pending transmit buffer. When the `fin` flag is set, it also
 *      notifies nghttp3 that the stream’s write side has been closed.
 *
 * Arguments:
 *      cc      - Connection context owning the nghttp3 session.
 *      sc      - Optional stream context (may be NULL if unavailable).
 *      h3_sid  - Stream ID in nghttp3 space.
 *      vecs    - Array of nghttp3_vec segments being skipped.
 *      nvec    - Number of entries in `vecs`.
 *      fin     - Non-zero if this skip implies stream completion (FIN).
 *      reason  - Short textual reason for logging.
 *
 * Results:
 *      Returns an H3DiscardState bitmask:
 *          H3_DISCARD_NONE       - nothing advanced or finalized
 *          H3_DISCARD_ADVANCED   - nghttp3 offset advanced
 *          H3_DISCARD_FIN        - stream write side closed
 *
 * Side effects:
 *      - Calls nghttp3_conn_add_write_offset() and possibly
 *        nghttp3_conn_shutdown_stream_write().
 *      - May trim bytes from sc->tx_pending via SharedTrimPending().
 *      - Updates sc->io_state and sc->seen_io if applicable.
 *      - Produces detailed diagnostic logs for debugging.
 *
 *----------------------------------------------------------------------
 */
static H3DiscardState
h3_stream_skip_write_and_trim(ConnCtx *cc, StreamCtx *sc,
                              int64_t h3_sid, nghttp3_vec *vecs, int nvec,
                              int fin, const char *reason)
{
    NsTLSConfig   *dc   = cc->dc;
    size_t         total = 0;
    H3DiscardState out   = H3_DISCARD_NONE;

    Ns_Log(Notice, "[%lld] H3[%lld] skip write: %s",
           (long long)dc->iter, (long long)h3_sid, reason);

    for (int i = 0; i < nvec; ++i) {
        total += vecs[i].len;
    }

    if (total > 0) {
        /* Advance nghttp3’s write offset by however many bytes we’re discarding. */
        nghttp3_conn_add_write_offset(cc->h3conn, h3_sid, total);
        out |= H3_DISCARD_ADVANCED;
    }

    if (sc == NULL) {
        /* No StreamCtx -> can’t trim pending, but still honour FIN below. */
        Ns_Log(Warning, "[%lld] H3[%lld] skip/discard without StreamCtx; pending not trimmed",
               (long long)dc->iter, (long long)h3_sid);
        if (fin) {
            out |= H3_DISCARD_FIN;
        }
    } else {
        /* Trim only bytes we had exposed in data_reader (tx_pending). */
        if (total > 0) {
            size_t pend    = SharedPendingUnreadBytes(&sc->sh);   /* or SharedPendingLen(&sc->sh) */
            size_t to_trim = (total < pend) ? total : pend;

            if (to_trim) {
                (void)SharedTrimPending(&sc->sh, to_trim, /*drain*/NS_FALSE);
                sc->seen_io = NS_TRUE;
            }
        }

        if (fin && !(sc->io_state & H3_IO_TX_FIN)) {
            sc->io_state |= H3_IO_TX_FIN;   /* local bookkeeping: we saw a FIN for this write */
            sc->seen_io   = NS_TRUE;
            out          |= H3_DISCARD_FIN;
        }
    }

    if (fin) {
        /* Tell nghttp3 that this stream’s write-side is done. */
        nghttp3_conn_shutdown_stream_write(cc->h3conn, h3_sid);
    }

    return out;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_read_data_cb --
 *
 *      nghttp3 data source callback that supplies outbound data to be
 *      framed into HTTP/3 DATA frames for a specific stream. This function
 *      is invoked by nghttp3 when it is ready to send application data for
 *      a stream whose body is produced asynchronously by the server.
 *
 *      The function inspects the stream’s SharedStream queues to determine
 *      if data is available:
 *
 *        - If EOF is already reached (no queued or pending bytes), it
 *          returns 0 with NGHTTP3_DATA_FLAG_EOF set.
 *        - If the stream has already been served during this write step
 *          (`tx_served_this_step`), it defers re-use by returning
 *          NGHTTP3_ERR_WOULDBLOCK.
 *        - If queued data is available but not yet pending, it moves bytes
 *          from the queued region into the pending buffer before building
 *          the output vecs.
 *        - When data is ready, it fills the provided nghttp3_vec array with
 *          read-only slices of the pending buffer and returns the count.
 *
 * Arguments:
 *      conn              - nghttp3 connection handle (unused here).
 *      stream_id         - HTTP/3 stream ID for which data is requested.
 *      vecs              - Output array to describe readable buffers.
 *      veccnt            - Maximum number of entries in `vecs`.
 *      flags             - Output flags; may include NGHTTP3_DATA_FLAG_EOF.
 *      conn_user_data    - Pointer to ConnCtx (set when connection was created).
 *      stream_user_data  - Pointer to StreamCtx (per-stream state).
 *
 * Results:
 *      Returns:
 *          >0 : number of nghttp3_vec entries filled (data available)
 *           0 : no data, but NGHTTP3_DATA_FLAG_EOF may be set (stream FIN)
 *         NGHTTP3_ERR_WOULDBLOCK : no data this tick, retry later
 *         NGHTTP3_ERR_* : fatal or flow control error (not used here)
 *
 * Side effects:
 *      - Moves data between shared queues via SharedSpliceQueuedToPending().
 *      - Marks the stream as `tx_served_this_step` to avoid redundant sends.
 *      - Emits detailed diagnostic logs about buffer state and data movement.
 *
 *----------------------------------------------------------------------
 */
static nghttp3_ssize
h3_stream_read_data_cb(nghttp3_conn   *UNUSED(conn),
                       int64_t         stream_id,
                       nghttp3_vec    *vecs,
                       size_t          veccnt,
                       uint32_t       *flags,
                       void           *conn_user_data,
                       void           *stream_user_data)
{
    ConnCtx       *cc = conn_user_data;
    StreamCtx     *sc = stream_user_data;
    SharedStream  *ss = &sc->sh;
    SharedSnapshot snap = SharedSnapshotInit(ss);

    assert(sc != NULL);

    Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_read_data_cb ENTER queued %ld pending %ld closed_by_app=%d veccnt %ld",
           (long long)cc->dc->iter, (long long)stream_id, snap.queued_bytes, snap.pending_bytes, snap.closed_by_app,
           veccnt);

    if (vecs == NULL || veccnt == 0) {
        return NGHTTP3_ERR_WOULDBLOCK;
    }

    if (sc->tx_served_this_step) {

        if (SharedEOFReady(&snap)) {
            *flags = NGHTTP3_DATA_FLAG_EOF;
            Ns_Log(Notice, "H3[%lld] h3_stream_read_data_cb: served earlier; now EOF", (long long)sc->h3_sid);
            return 0; /* 0 vecs + EOF => FIN */
        }

        Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_read_data_cb: already tx_served_this_step (queued %ld pending %ld closed by app %d)",
               (long long)cc->dc->iter, (long long)sc->quic_sid, snap.queued_bytes, snap.pending_bytes, snap.closed_by_app);

        *flags = 0;
        return NGHTTP3_ERR_WOULDBLOCK;   // don't re-offer in the same write step
    }

    /* Fast EOF: producer closed and no bytes left anywhere. */
    if (SharedEOFReady(&snap)) {
        *flags = NGHTTP3_DATA_FLAG_EOF;
        Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_read_data_cb: EOF (queues empty)", (long long)cc->dc->iter, (long long)stream_id);
        return 0;
    }

    /* Prime pending from queued if needed */
    if (SharedCanMove(&snap)) {
        size_t moved = SharedSpliceQueuedToPending(ss, SIZE_MAX);
        if (moved > 0) {
            Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_read_data_cb: moved %zu bytes queued -> pending",
                   (long long)cc->dc->iter, (long long)stream_id, moved);
            SharedSnapshotRead(&sc->sh, &snap);
        }
    }

    Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_read_data_cb SharedPendingUnreadBytes %ld",
           (long long)cc->dc->iter, (long long)stream_id, SharedPendingUnreadBytes(ss) );

    /* Nothing to send right now. */
    if (snap.pending_bytes == 0) {
        *flags = 0;
        Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_read_data_cb: no data, would block", (long long)cc->dc->iter, (long long)stream_id);
        return NGHTTP3_ERR_WOULDBLOCK;
    }

    {
        /* Build vecs from pending without mutating queues. */
        size_t out = SharedBuildVecsFromPending(ss, vecs, veccnt);

        SharedSnapshotRead(&sc->sh, &snap);
        Ns_Log(Notice,
               "[%lld] H3[%lld] h3_stream_read_data_cb: returning %zu vecs (%zu queued bytes; pending %zu)"
               " closed_by_app %d",
               (long long)cc->dc->iter, (long long)stream_id,
               out,
               snap.queued_bytes,
               snap.pending_bytes,
               snap.closed_by_app);

        sc->tx_served_this_step = NS_TRUE;
        Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_read_data_cb: mark tx_served_this_step", (long long)cc->dc->iter, (long long)sc->quic_sid);

        *flags = 0;
        return (nghttp3_ssize)out;
    }
}


/*======================================================================
 * Function Implementations: HTTP/3 Submit/Resume & Lifecycle
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_submit_ready_headers --
 *
 *      Submit an HTTP/3 response header block for a stream whose headers
 *      have been fully prepared in the shared header buffer. This function
 *      validates readiness, logs the outgoing headers, and calls
 *      nghttp3_conn_submit_response() to enqueue the HEADERS frame (and
 *      optionally attach a data reader for streaming the body).
 *
 *      The function is typically invoked from h3_conn_write_step() once the
 *      application has produced headers via SharedHdrsMarkReady(). It ensures
 *      that headers are only submitted once per stream, clears the header
 *      staging buffer afterward, and enables POLLOUT on the stream so that
 *      the write loop can send the resulting frames promptly.
 *
 * Results:
 *      Returns 0 on success, nonzero on error. Errors are logged internally.
 *
 * Side effects:
 *      - Calls nghttp3_conn_submit_response() with &sc->data_reader for
 *        body streams, or NULL for header-only responses (e.g. 204/304/HEAD).
 *      - Frees sc->resp_nv after submission; backing bytes remain in
 *        sc->resp_nv_store.
 *      - Clears SharedHdrs to reset readiness state.
 *      - For bodyless responses, immediately concludes the QUIC stream
 *        via SSL_stream_conclude().
 *      - Arms POLLOUT on the stream to schedule header transmission.
 *
 * Synchronization:
 *      - Protects sc->hdrs_submitted and header pointers via sc->lock.
 *
 *----------------------------------------------------------------------
 */
static int
h3_stream_submit_ready_headers(StreamCtx *sc)
{
    ConnCtx     *cc = sc->cc;
    NsTLSConfig *dc = cc->dc;
    int rv;

    /* Double-check under stream lock to publish/consume pointers safely */
    Ns_MutexLock(&sc->lock);

    if (sc->hdrs_submitted) {
        Ns_MutexUnlock(&sc->lock);
        SharedHdrsClear(&sc->sh);
        return 0;   /* already done */
    }

    if (!SharedHdrsIsReady(&sc->sh) || sc->resp_nv == NULL || sc->resp_nvlen == 0) {
        Ns_MutexUnlock(&sc->lock);
        SharedHdrsClear(&sc->sh);   /* defuse spurious edge */
        return -1;
    }

    h3_headers_log_nv(sc, sc->resp_nv, sc->resp_nvlen, "submit_response");

    /*
     * IMPORTANT: when a body is to be streamed via nghttp3 data source, we
     * have to pass &sc->data_reader.  If there is never any DATA (e.g.,
     * 304/204/HEAD or content-length:0), we can submit without a data source
     * and follow up by concluding the stream.
     */
    rv = nghttp3_conn_submit_response(cc->h3conn,
                                      h3_stream_id(sc),
                                      sc->resp_nv,
                                      sc->resp_nvlen,
                                      &sc->data_reader);   /* or NULL for header-only */
    Ns_Log(Notice, "[%lld] H3[%lld] submit_response nv=%zu -> %s",
           (long long)dc->iter, (long long)sc->h3_sid, sc->resp_nvlen, rv == 0 ? "OK" : "ERROR");

    if (rv != 0) {
        Ns_MutexUnlock(&sc->lock);
        SharedHdrsClear(&sc->sh);
        Ns_Log(Error, "[%lld] H3[%lld] nghttp3_conn_submit_response failed: %s",
               (long long)dc->iter, (long long)sc->h3_sid, nghttp3_strerror(rv));
        return -1;
    }

    sc->hdrs_submitted = NS_TRUE;

    /* We can free the array; backing bytes live in resp_nv_store */
    ns_free(sc->resp_nv);
    sc->resp_nv = NULL;
    sc->resp_nvlen = 0;

    Ns_MutexUnlock(&sc->lock);

    SharedHdrsClear(&sc->sh);

    /* If no body will be sent, conclude immediately with zero-length FIN. */
    if (!h3_response_has_body_now(sc)) {
        (void)SSL_stream_conclude(sc->ssl, 0);
        /* nghttp3 will need a resume tick to flush the FIN */
    }

    /* Keep per-stream POLLOUT armed; frames are pending */
    if (sc->ssl != NULL) {
        PollsetEnableWrite(dc, sc->ssl, sc, "submit_ready_headers");
    }

    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_drain --
 *
 *      Drain readable QUIC stream data into nghttp3, driving HTTP/3
 *      request or control frame processing. This function coordinates
 *      reading from OpenSSL’s QUIC stream (SSL_read_ex) and feeding the
 *      resulting bytes into nghttp3_conn_read_stream(), taking into account
 *      flow control, deferred delivery, and early gating before SETTINGS.
 *
 *      It operates in a staged loop:
 *        1. Feed any buffered bytes already held in sc->rx_hold.
 *        2. If more data is available, read from the QUIC stream into the
 *           staging buffer (via h3_stream_read_into_hold()).
 *        3. Repeat until all buffered data has been consumed or no more
 *           bytes are available from OpenSSL.
 *
 *      For client-initiated unidirectional streams, the function invokes
 *      h3_stream_maybe_note_uni_type() to recognize stream purpose before
 *      decoding data. For bidirectional streams, data feeding can be gated
 *      until the peer’s SETTINGS frame has been processed, to ensure proper
 *      synchronization.
 *
 * Results:
 *      Returns an H3DrainResultCode indicating stream state:
 *          DRAIN_PROGRESS - data read or fed successfully
 *          DRAIN_NONE     - no data currently available
 *          DRAIN_EOF      - stream reached end-of-file (FIN received)
 *          DRAIN_CLOSED   - stream context missing or already finished
 *          DRAIN_ERROR    - read or decode error occurred
 *
 * Side effects:
 *      - Calls SSL_read_ex() and nghttp3_conn_read_stream() as appropriate.
 *      - May update sc->rx_hold, rx_off, rx_len, and eof_seen flags.
 *      - Updates per-stream state and may trigger further nghttp3 processing.
 *      - Produces detailed logs for debugging stream-level flow.
 *
 *----------------------------------------------------------------------
 */
static H3DrainResultCode
h3_stream_drain(ConnCtx *cc, SSL *stream, uint64_t sid, const char *label)
{
    StreamCtx   *sc;
    int          gate_bidi;

    Ns_Log(Notice, "[%lld] H3[%llu] h3_stream_drain (%s)",
           (long long)cc->dc->iter, (unsigned long long)sid, label);


    if (stream == NULL) {
        return DRAIN_CLOSED;
    }

    // TODO: why no SSL_get_ex_data?
    //sc = SSL_get_ex_data(stream, dc->sc_idx);
    sc = StreamCtxGet(cc, (int64_t)sid, 0);
    if (sc == NULL || sc->eof_seen) {
        return DRAIN_CLOSED;
    }
    if (!SSL_has_pending(stream) && !sc->seen_readable && sc->rx_len == sc->rx_off) {
        return DRAIN_NONE;
    }
    StreamCtxRequireRxBuffer(sc);

    /*
     * If bidi & SETTINGS not yet processed, we still stage bytes (so we don’t
     * lose anything) but we never feed them to nghttp3 until control
     * processed.
     */
    gate_bidi = (StreamCtxIsBidi(sc) && !cc->settings_seen);

    sc->rx_emitted_in_pass = 0;

    for (;;) {
        H3DrainResultCode dr;

        /* Feed any pending bytes first (unless gated) */
        if (!gate_bidi && sc->rx_off < sc->rx_len) {
            H3FeedResultCode fr;

            if (StreamCtxIsClientUni(sc)) {
                h3_stream_maybe_note_uni_type(sc, stream, sid);
            }

            fr = h3_stream_feed_pending(sc, sid);
            Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_drain h3_stream_feed_pending %s",
                   (long long)cc->dc->iter, (long long)sc->quic_sid, H3FeedResultCode_str(fr));

            if (fr == FEED_ERR)   return DRAIN_ERROR;
            if (fr == FEED_EOF)   return DRAIN_EOF;
            if (fr == FEED_OK_BLOCKED) return DRAIN_PROGRESS;  /* nghttp3 said rv==0 */

            /* FEED_OK_PROGRESS: keep looping to read more or feed more */
            if (gate_bidi) return DRAIN_PROGRESS; /* shouldn’t happen, but be strict */
        }

        /* f we still have bytes staged (e.g., gated bidi), we can’t read again. */
        if (sc->rx_off < sc->rx_len) return DRAIN_PROGRESS;

        /* Stage more from TLS if window empty */
        dr = h3_stream_read_into_hold(sc, stream);
        //Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_drain h3_stream_read_into_hold %s",
        //       (long long)cc->dc->iter, (long long)sc->quic_sid, H3DrainResultCode_str(dr));
        if (dr == DRAIN_ERROR || dr == DRAIN_EOF || dr == DRAIN_NONE) {
            return dr;
        }

        /* dr == DRAIN_PROGRESS: we have fresh bytes */
        if (gate_bidi) {
            return DRAIN_PROGRESS;
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_maybe_finalize --
 *
 *      Drive the finalization process of an HTTP/3 stream and ensure that
 *      write interest (W) is managed consistently in terminal states. This
 *      function is designed to prevent "sticky" write events and ensures that
 *      FIN emission and cleanup are always performed safely within the owning
 *      thread.
 *
 * Rationale:
 *      - RESET first: If the stream is already in a reset state
 *        (H3_IO_RESET), we immediately drop write interest and, if the RX
 *        side is closed or there’s nothing left to send, mark the stream as
 *        dead. This avoids lingering EW flags in terminal states.
 *
 *      - Empty FIN emission: When `close_when_drained` is set and there's no
 *        pending or queued TX data, we send an empty FIN frame
 *        (`SSL_write_ex2(..., NULL, 0, SSL_WRITE_FLAG_CONCLUDE)`).  FIN is
 *        only sent if TX isn't already concluded, the TX path is still
 *        writable, and the app hasn't locally closed TX.  This ensures that
 *        FINs are only emitted in one place: here.
 *
 *      - Interest gating: W events are only armed if `H3_TX_WRITABLE(sc)` is
 *        NS_TRUE. This guarantees we never re-arm EW if our local state
 *        considers TX already closed.
 *
 *      - Single arm/disarm decision: The final write-interest decision is
 *        deferred until after any FIN attempt, so we don’t flap W within the
 *        same scheduling tick.
 *
 * Returns:
 *      NS_TRUE  - The stream is fully finalized and has been marked dead.
 *      NS_FALSE - The stream is still active or awaiting further I/O.
 *
 * Side Effects:
 *      May emit an empty FIN frame via SSL_write_ex2().
 *      May enable or disable W events for the stream.
 *      May mark the stream dead when both TX and RX are closed.
 *
 *----------------------------------------------------------------------
 */

static bool
h3_stream_maybe_finalize(StreamCtx *sc, const char *label)
{
    ConnCtx        *cc = sc->cc;
    NsTLSConfig    *dc = cc->dc;
    bool            finalized = NS_FALSE;
    bool            want_w_prev, has_tx, need_w;
    SharedSnapshot snap = SharedSnapshotInit(&sc->sh);

    has_tx = SharedHasData(&snap);

    Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_maybe_finalize called %s (%s)",
           (long long)dc->iter, (long long)sc->quic_sid, H3StreamKind_str(sc->kind), label);

    /* --- hard terminal? handle RESET first --- */
    if (H3_IO_HAS(sc, H3_IO_RESET)) {
        PollsetDisableWrite(dc, sc->ssl, sc, "h3_stream_maybe_finalize: reset");
        if (H3_RX_CLOSED(sc) || !has_tx) {
            PollsetMarkDead(cc, sc->ssl, "h3_stream_maybe_finalize: reset");
            finalized = NS_TRUE;
        }
        Ns_Log(Notice, "[%lld] h3_stream_maybe_finalize %p %s %s RESET returns %d",
               (long long)dc->iter, (void*)sc->ssl, label, H3StreamKind_str(sc->kind), finalized);
        return finalized;
    }

    /* ---- lazy close path: only if we never concluded via nghttp3 ---- */
    if (StreamCtxIsServerUni(sc) && SharedEOFReady(&snap) && !H3_IO_HAS(sc, H3_IO_TX_FIN)) {
        int ok = SSL_stream_conclude(sc->ssl, 0);
        Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_maybe_finalize %s %s stream_conclude returns %d",
               (long long)dc->iter, (long long)sc->quic_sid, label, H3StreamKind_str(sc->kind), ok);
        if (ok == 1) {
            sc->io_state |= H3_IO_TX_FIN;

        } else {
            int err = SSL_get_error(sc->ssl, ok);

            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                /* try again in a later tick */
            } else if (err == SSL_ERROR_SSL) {
                unsigned long e = ERR_peek_error();
                if (ERR_GET_LIB(e) == ERR_LIB_SSL &&
                    ERR_GET_REASON(e) == SSL_R_PROTOCOL_IS_SHUTDOWN) {
                    /* peer already considers it closed – treat as FIN */
                    sc->io_state |= H3_IO_TX_FIN;
                    ERR_clear_error();
                }
            }
            /* IMPORTANT: no "hard failure"/RESET here */
        }
    }

    (void)SSL_handle_events(sc->ssl);          /* harmless if nothing pending */

    /* --- recompute snapshot after potential FIN attempt --- */
    SharedSnapshotRead(&sc->sh, &snap);
    has_tx = SharedHasData(&snap);

    /* --- final write-interest decision for this tick --- */
    Ns_MutexLock(&sc->lock);
    want_w_prev     = sc->wants_write;  /* one-shot snapshot */
    sc->wants_write = NS_FALSE;         /* clear one shot */
    Ns_MutexUnlock(&sc->lock);

    /* Keep W if app still has bytes OR we just asked to write OR a FIN is still pending */

    need_w = has_tx || want_w_prev || (sc->hdrs_submitted && !sc->eof_sent);

    Ns_Log(Notice,
           "[%lld] H3[%lld] h3_stream_maybe_finalize reads sc->wants_write %d need_w %d"
           " has_tx %d (queued %zu pending %zu)",
           (long long)dc->iter, (long long)sc->quic_sid, want_w_prev, need_w, has_tx, snap.queued_bytes, snap.pending_bytes);

    if (need_w) {
        Ns_Log(Notice,
               "[%lld] H3[%lld] h3_stream_maybe_finalize need W: closed_by_app %d io_state %.2x",
               (long long)dc->iter, (long long)sc->quic_sid, snap.closed_by_app, sc->io_state);
        PollsetEnableWrite(dc, sc->ssl, sc, "h3_stream_maybe_finalize: need W");
    } else if (sc->seen_io) {
        PollsetDisableWrite(dc, sc->ssl, sc, "h3_stream_maybe_finalize: idle");
    }

    /* ---- reap using our own flags + buffers ---- */
    if (H3_BOTH_CLOSED(sc) && SharedIsEmpty(&snap)) {
        /* we’re done with this BIDI stream */
        PollsetDisableRead (dc, sc->ssl, sc, "h3_stream_maybe_finalize: both-closed");
        PollsetDisableWrite(dc, sc->ssl, sc, "h3_stream_maybe_finalize: both-closed");
        PollsetMarkDead(cc, sc->ssl, "h3_stream_maybe_finalize: both-closed");
        return NS_TRUE;
    }

    Ns_Log(Notice, "[%lld] H3[%lld] h3_stream_maybe_finalize %p %s %s returns %d",
           (long long)dc->iter, (long long)sc->quic_sid, (void*)sc->ssl, label, H3StreamKind_str(sc->kind), finalized);

    return finalized;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_can_free --
 *
 *      Determine whether a stream’s resources can be safely freed.
 *      A stream becomes eligible for cleanup once both the receive (RX)
 *      and transmit (TX) halves have completed (i.e., FIN observed on
 *      both sides), or if the stream has been reset due to an error or
 *      protocol condition.
 *
 * Results:
 *      Returns true if the stream may be freed, false otherwise.
 *
 *----------------------------------------------------------------------
 */
static inline bool h3_stream_can_free(const StreamCtx *sc) {
    return ((sc->io_state & (H3_IO_RX_FIN|H3_IO_TX_FIN)) == (H3_IO_RX_FIN|H3_IO_TX_FIN))
        || (sc->io_state & H3_IO_RESET);
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_maybe_note_uni_type --
 *
 *      Detect and record the type of an incoming client-initiated
 *      unidirectional (uni) stream. HTTP/3 specifies that the first
 *      varint in such streams identifies its purpose - e.g.,
 *      CONTROL (0x00), QPACK encoder (0x02), QPACK decoder (0x03),
 *      or an unknown/extension type.
 *
 *      This function peeks at that leading varint without consuming it
 *      (i.e., without advancing sc->rx_off), stores the decoded type in
 *      sc->uni_type, and updates the connection context accordingly so
 *      the stream can be recognized later.
 *
 * Arguments:
 *      sc      - Stream context (must represent a client-initiated UNI stream).
 *      stream  - Associated SSL* QUIC stream handle.
 *      sid     - QUIC stream ID.
 *
 * Results:
 *      None (void). Updates the stream and connection context in place.
 *
 * Side effects:
 *      - Sets sc->type_consumed = true once type is decoded.
 *      - Records type-specific stream pointers in ConnCtx (e.g., client_control_ssl,
 *        client_qpack_enc_ssl, client_qpack_dec_ssl).
 *      - Marks sc->ignore_uni = true for QPACK or unknown/extension streams.
 *      - Does *not* advance rx_off; the type byte remains available to nghttp3.
 *
 *----------------------------------------------------------------------
 */
static void
h3_stream_maybe_note_uni_type(StreamCtx *sc, SSL *stream, uint64_t sid)
{
    if (StreamCtxIsClientUni(sc) && !sc->type_consumed && sc->rx_off < sc->rx_len) {
        size_t avail = sc->rx_len - sc->rx_off;
        size_t vtlen = quic_varint_len(sc->rx_hold[sc->rx_off]);

        if (vtlen > 0 && vtlen <= avail) {
            uint64_t stype = quic_varint_decode(sc->rx_hold + sc->rx_off, avail);
            ConnCtx *cc = sc->cc;

            sc->type_consumed = NS_TRUE;
            sc->uni_type = stype;

            if (stype == 0x00) {               /* CONTROL */
                cc->client_control_sid = sid;
                cc->client_control_ssl = stream;
            } else if (stype == 0x02) {        /* QPACK encoder */
                cc->client_qpack_enc_sid = sid;
                cc->client_qpack_enc_ssl = stream;
                sc->ignore_uni = NS_TRUE;
            } else if (stype == 0x03) {        /* QPACK decoder */
                cc->client_qpack_dec_sid = sid;
                cc->client_qpack_dec_ssl = stream;
                sc->ignore_uni = NS_TRUE;
            } else {
                sc->ignore_uni = NS_TRUE;      /* GREASE/unknown */
            }
            /* IMPORTANT: do NOT advance rx_off here; nghttp3 must see the type byte. */
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * h3_conn_wake --
 *
 *      Platform-neutral wakeup helper for the HTTP/3 listener loop.
 *      Since OpenSSL’s QUIC poll integration currently lacks support
 *      for a trigger pipe or eventfd-style wake mechanism, this function
 *      sends a one-byte UDP datagram to the listener’s bound address to
 *      interrupt blocking poll/select calls.
 *
 * Arguments:
 *      dc - The TLS configuration for the HTTP/3 driver, containing the
 *           waker socket address and length in dc->u.h3.waker_addr.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Opens a temporary UDP socket and transmits a single dummy byte
 *        to the listener’s waker address.
 *      - Causes the listener’s poll loop to wake and resume processing.
 *      - Closes the temporary socket immediately after sending.
 *
 *----------------------------------------------------------------------
 */
inline void h3_conn_wake(NsTLSConfig *dc) {
    const struct sockaddr *sa = (const struct sockaddr *)&(dc->u.h3.waker_addr);

    if (dc->u.h3.waker_addrlen > 0) {
        int                 fd = socket(sa->sa_family, SOCK_DGRAM, 0);
        const unsigned char b = 0; /* not a QUIC header byte */

        if (fd < 0) {
            return;
        }
        Ns_Log(Notice, "[%lld] H3: h3_conn_wake", (long long)dc->iter);

        (void)sendto(fd, (const char *)&b, 1, 0, sa, dc->u.h3.waker_addrlen);
        ns_sockclose(fd);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * h3_stream_id --
 *
 *      Return the appropriate stream identifier to use in nghttp3 calls.
 *      Each StreamCtx tracks both a QUIC stream ID (quic_sid) and, once
 *      initialized, an nghttp3 stream ID (h3_sid). During the early setup
 *      phase, before nghttp3 assigns its own ID, h3_sid may still be
 *      negative. In that case, this helper falls back to the underlying
 *      QUIC stream ID.
 *
 * Arguments:
 *      sc - Stream context whose identifiers are queried.
 *
 * Results:
 *      Returns the nghttp3 stream ID if available (h3_sid >= 0),
 *      otherwise the QUIC stream ID as a fallback.
 *
 *----------------------------------------------------------------------
 */
static inline int64_t h3_stream_id(const StreamCtx *sc) {
    return sc->h3_sid >= 0 ? sc->h3_sid : (int64_t)sc->quic_sid;
}

/*======================================================================
 * Function Implementations: HTTP/3 Response Body Management
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * h3_response_allows_body --
 *
 *      Determine whether an HTTP/3 response with the given status code
 *      and request method is permitted to carry a message body.
 *
 *      Implements RFC 9110 rules:
 *        - HEAD responses never include a body.
 *        - 1xx (informational), 204 (No Content), and 304 (Not Modified)
 *          responses must not include a body.
 *        - Successful CONNECT (2xx) responses also omit a body.
 *
 * Results:
 *      NS_FALSE if a body is disallowed for this response,
 *      NS_TRUE otherwise.
 *
 *----------------------------------------------------------------------
 */
static inline bool
h3_response_allows_body(int status, const char *method)
{
    if (STREQ(method, "HEAD")) {
        return NS_FALSE;
    }

    /* RFC 9110: 1xx, 204, 304 never have a content */
    if ((status >= 100 && status < 200) || status == 204 || status == 304) {
        return NS_FALSE;                                    // RFC: no-body statuses
    }

    /* CONNECT successful responses (2xx) typically have no message body */
    if (STREQ(method, "CONNECT") && status >= 200 && status < 300) {
        return NS_FALSE;
    }
    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * h3_response_has_body_now --
 *
 *      Determine whether the current HTTP/3 stream should emit a message
 *      body at this point in time.
 *
 *      This helper inspects both protocol-level and application-level
 *      indicators to decide whether a DATA section is expected:
 *        - If the response type forbids a body (per status or method),
 *          returns false immediately.
 *        - If the application has already queued or is streaming data,
 *          returns NS_TRUE.
 *        - If the response includes a non-zero content-length header,
 *          returns true even if data is not yet queued.
 *        - Otherwise, the response is treated as bodyless (zero-length).
 *
 * Results:
 *      NS_TRUE  if the response currently has (or will have) a body.
 *      NS_FALSE if the response is bodyless or finished.
 *
 *----------------------------------------------------------------------
 */
static inline bool
h3_response_has_body_now(StreamCtx *sc)
{
    if (!sc->response_allow_body) {
        return NS_FALSE;

    } else {
        SharedSnapshot snap = SharedSnapshotInit(&sc->sh);

        // If the app has enqueued or is producing, we have a body.
        if (SharedHasData(&snap) /* TODO: for streaming: || sc->has_body_producer */) {
            return NS_TRUE;
        }

        // If the app explicitly set content-length:
        if (sc->response_has_non_zero_content_length) {
            return NS_TRUE;
        }
    }

    // No producer and nothing queued -> no body (zero-length).
    return NS_FALSE;
}

/*======================================================================
 * Function Implementations: HTTP/3 Diagnostics / Stringifiers
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * H3DrainResultCode_str, H3FeedResultCode_str, H3StreamKind_str --
 *
 *      Utility functions returning human-readable string names for
 *      internal HTTP/3 enumerations used in diagnostics and logs.
 *
 *      - H3DrainResultCode_str(): Converts a H3DrainResultCode (result of
 *        draining received data) into a descriptive string.
 *
 *      - H3FeedResultCode_str(): Converts a H3FeedResultCode (result of
 *        feeding buffered data into nghttp3) into a descriptive string.
 *
 *      - H3StreamKind_str(): Converts a H3StreamKind enum (logical stream
 *        role such as control, QPACK, or request stream) into a symbolic
 *        label.
 *
 * Results:
 *      Returns a constant string identifying the respective enum value,
 *      or a fallback "UNKNOWN" string if the code is not recognized.
 *
 *----------------------------------------------------------------------
 */
static const char *H3DrainResultCode_str(H3DrainResultCode dr)
{
    switch (dr) {
    case DRAIN_NONE:     return "DRAIN_NONE";
    case DRAIN_PROGRESS: return "DRAIN_PROGRESS";
    case DRAIN_EOF:      return "DRAIN_EOF";
    case DRAIN_CLOSED:   return "DRAIN_CLOSED";
    case DRAIN_ERROR:    return "DRAIN_ERROR";
    }
    return "DRAIN_RESULT_UNKNOWN";
}
static const char *H3FeedResultCode_str(H3FeedResultCode fr)
{
    switch (fr) {
    case FEED_OK_PROGRESS: return "FEED_OK_PROGRESS";
    case FEED_OK_BLOCKED:  return "FEED_OK_BLOCKED";
    case FEED_EOF:         return "FEED_EOF";
    case FEED_ERR:         return "FEED_ERR";
    }
    return "FEED_RESULT_UNKNOWN";
}

static const char *H3StreamKind_str(H3StreamKind kind)
{
    switch (kind) {
    case H3_KIND_UNKNOWN:          return "H3_KIND_UNKNOWN";
    case H3_KIND_CTRL:             return "H3_KIND_CTRL";
    case H3_KIND_QPACK_ENCODER:    return "H3_KIND_QPACK_ENCODER";
    case H3_KIND_QPACK_DECODER:    return "H3_KIND_QPACK_DECODER";
    case H3_KIND_CLIENT_UNI:       return "H3_KIND_CLIENT_UNI";
    case H3_KIND_BIDI_REQ:         return "H3_KIND_BIDI_REQ";
    }
    return "STREAM_KIND_UNKNOWN";
}

/* H3 / nghttp3 allocator hooks */
/*
 *----------------------------------------------------------------------
 *
 * h3_malloc_cb, h3_free_cb, h3_calloc_cb, h3_realloc_cb --
 *
 *      Custom memory allocator hooks for nghttp3, wired to NaviServer’s
 *      memory management layer. These callbacks allow nghttp3 to use
 *      ns_malloc/ns_free/ns_calloc/ns_realloc internally instead of the
 *      system allocator, ensuring consistent memory tracking and
 *      compatibility with NaviServer’s runtime environment.
 *
 * Results:
 *      Return allocated or reallocated memory on success, or NULL on failure.
 *      h3_free_cb() releases previously allocated memory.
 *
 *----------------------------------------------------------------------
 */

static void *h3_malloc_cb(size_t size, void *UNUSED(user_data)) {
    return ns_malloc(size);
}

static void h3_free_cb(void *ptr, void *UNUSED(user_data)) {
    ns_free(ptr);
}

static void *h3_calloc_cb(size_t count, size_t size, void *UNUSED(user_data)) {
    return ns_calloc(count, size);
}

static void *h3_realloc_cb(void *ptr, size_t size, void *UNUSED(user_data)) {
    return ns_realloc(ptr, size);
}

/*======================================================================
 * Function Implementations: HTTP/3 nghttp3 Callbacks
 *======================================================================
 */

/*
 * on_recv_settings --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_recv_settings' (see
 *      https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).  Called
 *      when a peer’s SETTINGS frame is received, updating local connection
 *      limits such as max_field_section_size and marking SETTINGS as
 *      processed.
 */
static int on_recv_settings(nghttp3_conn *UNUSED(conn),
                            const nghttp3_settings *s,
                            void *user_data)
{
    ConnCtx *cc = user_data;

    cc->client_max_field_section_size = s->max_field_section_size;
    cc->settings_seen = NS_TRUE;

    Ns_Log(Notice,
           "H3 on_recv_settings: max_field_section_size=%llu, "
           "qpack_max_dtable=%llu, qpack_blocked=%u",
           (unsigned long long)s->max_field_section_size,
           (unsigned long long)s->qpack_max_dtable_capacity,
           (unsigned)s->qpack_blocked_streams);
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * on_begin_headers --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_begin_headers'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Invoked when a new HEADERS frame begins on a stream.  Initializes
 *      or retrieves the corresponding StreamCtx, installs its data reader
 *      callback, and associates the StreamCtx with the stream’s user data
 *      via nghttp3_conn_set_stream_user_data().
 *
 *----------------------------------------------------------------------
 */
static int on_begin_headers(nghttp3_conn *UNUSED(conn), int64_t stream_id,
                            void *conn_ud, void *UNUSED(stream_user_data))
{
    ConnCtx   *cc = conn_ud;
    StreamCtx *sc = StreamCtxGet(cc, stream_id, 0);
    int        rv;

    if (sc == NULL) {
        Ns_Log(Notice, "H3[%lld] on_begin_headers sc missing", (long long)stream_id);
        return NGHTTP3_ERR_NOMEM;
    }

    // Initialize only the essential parts here
    sc->h3_sid = stream_id;
    //sc->rx_emitted_in_pass = 0;
    assert(sc->h3_sid == (int64_t)sc->quic_sid);

    memset(&sc->data_reader, 0, sizeof(sc->data_reader));
    sc->data_reader.read_data = h3_stream_read_data_cb;
    Ns_Log(Notice, "H3[%lld] on_begin_headers set h3_stream_read_data_cb for stream_ctx %p", (long long)stream_id , (void*)sc);

    /*
     * Attach the sc to the stream user data
     */
    rv = nghttp3_conn_set_stream_user_data(cc->h3conn, stream_id, sc);
    //Ns_Log(Notice, "H3 setting stream_user_data for %lld to %p", (long long)stream_id, (void*)sc);
    if (rv != 0) {
        /* cleanup resources */
        StreamCtxUnregister(sc);
        // Close the stream with error - new nghttp3 API uses two arguments
        nghttp3_conn_close_stream(cc->h3conn, stream_id, NGHTTP3_H3_INTERNAL_ERROR);
        return rv;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * on_recv_header --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_recv_header'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Invoked for each name/value pair in a HEADERS frame.  Parses and
 *      stores pseudo-headers (e.g., :method, :path, :authority, :scheme)
 *      into the StreamCtx, and appends regular header fields into the
 *      request’s header set.  Also tracks "Host" and "content-length"
 *      for HTTP/3 request handling.
 *
 *----------------------------------------------------------------------
 */

static int on_recv_header(nghttp3_conn *UNUSED(conn), int64_t UNUSED(stream_id), int32_t UNUSED(token),
                          nghttp3_rcbuf *name, nghttp3_rcbuf *value,
                          uint8_t UNUSED(flags), void *UNUSED(conn_user_data), void *stream_user_data)
{
    StreamCtx  *sc = stream_user_data;
    nghttp3_vec n = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec v = nghttp3_rcbuf_get_buf(value);

    if (n.len > 0 && n.base[0] == ':') {
        /* Handle pseudo-headers */
        if (n.len == 7 && memcmp(n.base, ":method", 7)==0) {
            sc->method   = strndup((char*)v.base, v.len);

        } else if (n.len == 5 && memcmp(n.base, ":path",   5)==0) {
            sc->path = strndup((char*)v.base, v.len);

        } else if (n.len == 10 && memcmp(n.base, ":authority",10)==0) {
            sc->authority = strndup((char*)v.base, v.len);

        } else if (n.len == 7 && memcmp(n.base, ":scheme", 7)==0) {
            sc->scheme = strndup((char*)v.base, v.len);
        }
    } else {
        /* classical header fields, keep the fields in the chunks */
        Ns_Set *hdrs = SockEnsureReqHeaders(sc);

        if (n.len == 4 && memcmp(n.base, "host", 4) == 0) {
            sc->saw_host_header = NS_TRUE;

        } else if (n.len == 14 && memcmp(n.base, "content-length", 14) == 0) {
            long long cl = 0;

            for (size_t i = 0; i < v.len; i++) {
                if (v.base[i] >= '0' && v.base[i] <= '9') {
                    cl = cl * 10 + (v.base[i]-'0');
                } else {
                    cl = -1;
                    break;
                }
            }
            if (cl >= 0) {
                Sock *sockPtr = (Sock*)sc->nsSock;
                sockPtr->reqPtr->contentLength = (size_t)cl;
            }
        }

        Ns_SetPutSz(hdrs, (const char*)n.base, (TCL_SIZE_T)n.len, (const char*)v.base, (TCL_SIZE_T)v.len);
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * on_end_headers --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_end_headers'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Invoked when all HEADERS for a stream have been received.
 *      Finalizes request setup by ensuring a valid Host header,
 *      parsing the request line, and logging request details.
 *      If no request body is expected, the request is dispatched
 *      immediately; otherwise, the function prepares for body
 *      reception, choosing between in-memory or temporary-file
 *      buffering depending on upload size limits.
 *
 *----------------------------------------------------------------------
 */
static int
on_end_headers(nghttp3_conn *UNUSED(conn), int64_t stream_id, int fin,
               void *conn_user_data, void *stream_user_data)
{
    ConnCtx     *cc = conn_user_data;
    StreamCtx   *sc = stream_user_data;
    NsTLSConfig *dc = cc->dc;
    Sock        *sockPtr = (Sock *)sc->nsSock;
    Request     *reqPtr;
    Ns_Set      *hdrs;
    bool         has_body;
    bool         has_content_length;
    char         peer[NS_IPADDR_SIZE];

    Ns_Log(Debug, "H3[%lld] on_end_headers fin %d", (long long)stream_id, fin);

    /* Make sure we have an NsRequest + header set */
    hdrs   = SockEnsureReqHeaders(sc);
    reqPtr = sockPtr->reqPtr;
    (void)ns_inet_ntop((struct sockaddr *)&(sockPtr->sa), peer, NS_IPADDR_SIZE);

    /* Ensure Host header exists (map :authority -> Host if needed) */
    if (!sc->saw_host_header && sc->authority != NULL) {
        Ns_SetPutSz(hdrs, "host", 4, sc->authority, (TCL_SIZE_T)strlen(sc->authority));
        sc->saw_host_header = NS_TRUE;
    }

    has_content_length = Ns_SetFind(hdrs, "content-length") > -1;
    has_body           = has_content_length && (reqPtr->contentLength > 0);
    {
        Tcl_DString  line;

        Tcl_DStringInit(&line);
        Tcl_DStringAppend(&line, sc->method ? sc->method : "GET", TCL_INDEX_NONE);
        Tcl_DStringAppend(&line, " ", 1);
        Tcl_DStringAppend(&line, sc->path ? sc->path : "/", TCL_INDEX_NONE);
        Tcl_DStringAppend(&line, " HTTP/1.1", 9);

        Ns_Log(Notice, "H3[%lld] on_end_headers peer %s request line: %s",
               (long long)stream_id, peer, line.string);
        if (Ns_ParseRequest(&reqPtr->request, line.string, (size_t)line.length) != NS_OK) {
            Tcl_DStringFree(&line);
            Ns_Log(Warning, "H3[%lld] GET/HEAD fastpath: Ns_ParseRequest failed for peer %s '%s'",
                   (long long)stream_id, peer, line.string);
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        Tcl_DStringFree(&line);
    }

    Ns_Log(Debug, "[%lld] H3 on_end_headers req %p line '%s'",
           (long long)dc->iter, (void*)sockPtr->reqPtr, sockPtr->reqPtr->request.line);

    { Tcl_DString ds;
        Tcl_DStringInit(&ds);
        Ns_Log(Notice, "H3[%lld] on_end_headers fin %d has_content_length %d reqPtr->contentLength %ld has_body %d peer %s %s",
               (long long)stream_id, fin, has_content_length, reqPtr->contentLength, has_body,
               peer, Ns_SetFormat(&ds, hdrs, NS_TRUE, "", ": "));
        Tcl_DStringFree(&ds);
    }

    reqPtr->coff    = 1;   /* "past headers"; any non-zero is OK */
    reqPtr->length  = 0;
    reqPtr->avail   = 0;
    reqPtr->content = NULL;
    reqPtr->next    = NULL;
    //sockPtr->keep   = NS_TRUE;    // not sure about this

    if (!has_body) {
        Ns_Log(Debug, "H3[%lld] on_end_headers, no body (sockPtr %p) ip %s",
               (long long)stream_id, (void*)sockPtr, peer);
        if (NsDispatchRequest(sockPtr) != NS_OK) {
            Ns_Log(Warning, "H3 NsDispatchRequest (GET/HEAD fastpath) failed");
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        } else {
            sc->io_state |= H3_IO_REQ_DISPATCHED;
        }

    } else {
        /* Choose sink: memory for small bodies, temp file for larger/unknown */
        const Driver *drvPtr = sockPtr->drvPtr;

        sockPtr->tfile = NULL;
        sockPtr->tfd = NS_INVALID_FD;

        Ns_Log(Notice, "H3[%lld] on_end_headers request with body size %ld maxupload %ld",
               (long long)stream_id, reqPtr->contentLength, drvPtr->maxupload);

        if (drvPtr->maxupload > 0
            && reqPtr->contentLength > (size_t)drvPtr->maxupload
            ) {
            size_t tfileLength = strlen(drvPtr->uploadpath) + 16u;

            sockPtr->tfile = ns_malloc(tfileLength);
            snprintf(sockPtr->tfile, tfileLength, "%s/%d.XXXXXX", drvPtr->uploadpath, sockPtr->sock);
            sockPtr->tfd = ns_mkstemp(sockPtr->tfile);
            Ns_Log(Notice, "H3[%lld] on_end_headers fin %d has_body %d submit via fd %d", (long long)stream_id, fin, has_body, sockPtr->tfd);

            if (sockPtr->tfd == NS_INVALID_FD) {
                Ns_Log(Error, "SockRead: cannot create spool file with template '%s': %s",
                       sockPtr->tfile, strerror(errno));
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
        } else {
        }
    }
    /*
     * If we receive content, and sockPtr->tfd != NS_INVALID_FD, spool to a
     * file, otherwise provide data in memory.
     */
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * on_recv_data --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_recv_data'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Called when a DATA frame is received for a stream.  Appends the
 *      received payload either to the request’s in-memory buffer or to
 *      a temporary file (if spooling is active), and updates the total
 *      body length.  When the declared content-length is fully received,
 *      marks the stream as ready for request dispatch (H3_IO_REQ_READY).
 *
 *----------------------------------------------------------------------
 */
static int on_recv_data(nghttp3_conn *UNUSED(conn), int64_t stream_id,
                        const uint8_t *data, size_t datalen,
                        void *conn_user_data, void *stream_user_data)
{
    ConnCtx   *cc = conn_user_data;
    StreamCtx *sc = stream_user_data;
    Sock      *sockPtr = (Sock *)sc->nsSock;
    Ns_Set    *hdrs    = SockEnsureReqHeaders(sc);
    Request   *reqPtr  = sockPtr->reqPtr;
    bool       has_content_length;
    int        result = 0;

    Ns_Log(Notice, "[%lld] H3[%lld] on_recv_data datalen %ld  sc %p old sid %lld, new sid %lld (emitted_in_pass %ld)",
           (long long)cc->dc->iter, (long long)stream_id, datalen, (void*) sc, (long long)sc->h3_sid, (long long)stream_id, sc->rx_emitted_in_pass);
    sc->h3_sid = stream_id;

    if (datalen > 0) {
        sc->rx_emitted_in_pass += datalen;
        if (sockPtr->tfd != NS_INVALID_FD) {
            ssize_t wr;
            Ns_Log(Notice, "[%lld] H3[%lld] on_recv_data write to file %ld bytes", (long long)cc->dc->iter, (long long)sc->quic_sid, datalen);
            wr = ns_write(sockPtr->tfd, data, datalen);
            if (wr < 0 || (size_t)wr != datalen) {
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
        } else {
            Ns_Log(Notice, "[%lld] H3[%lld] on_recv_data append to buffer %ld bytes", (long long)cc->dc->iter, (long long)sc->quic_sid, datalen);
            Tcl_DStringAppend(&reqPtr->buffer, (const char*)data, (TCL_SIZE_T)datalen);
        }

        reqPtr->length += datalen;             /* total body bytes received */
    }

    has_content_length = Ns_SetFind(hdrs, "content-length") > -1;

    if (has_content_length && reqPtr->length >= reqPtr->contentLength) {
        Ns_Log(Notice, "[%lld] H3[%lld] on_recv_data sets H3_IO_REQ_READY",
               (long long)cc->dc->iter, (long long)stream_id);
        sc->io_state |= H3_IO_REQ_READY;
    }

    Ns_Log(Notice, "[%lld] H3[%lld] on_recv_data received +%zu (total %ld/%ld) -> result %d",
           (long long)cc->dc->iter, (long long)sc->h3_sid, datalen, reqPtr->length, reqPtr->contentLength, result);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * on_end_stream --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_end_stream'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Invoked when the peer signals the end of a stream (FIN).  Marks
 *      the receive half as finished (H3_IO_RX_FIN) and sets eof_seen,
 *      indicating that the request body is complete and ready for
 *      dispatch (H3_IO_REQ_READY).
 *
 *----------------------------------------------------------------------
 */
static int on_end_stream(nghttp3_conn *UNUSED(conn), int64_t stream_id,
                         void *UNUSED(conn_user_data), void *stream_user_data)
{
    StreamCtx *sc = stream_user_data;

    Ns_Log(Notice, "[%lld] H3[%lld] on_end_stream", (long long)sc->cc->dc->iter, (long long)stream_id);
    assert(sc != NULL);

    /* Protocol-level end of the peer's send side */
    sc->io_state |= H3_IO_RX_FIN;

    /* Provide eof_seen as an HTTP-layer "request complete" hint */
    sc->eof_seen = NS_TRUE;

    Ns_Log(Notice, "[%lld] H3[%lld] on_end_stream sets H3_IO_REQ_READY",
           (long long)sc->cc->dc->iter, (long long)stream_id);

    sc->io_state |= H3_IO_REQ_READY;
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * on_acked_stream_data --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_acked_stream_data'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Called when a portion of previously sent stream data has been
 *      acknowledged by the peer.  Clears flow-control blockage flags and
 *      resumes the corresponding stream in nghttp3 to allow further data
 *      transmission.
 *
 *----------------------------------------------------------------------
 */
static int on_acked_stream_data(nghttp3_conn *UNUSED(conn),
                                int64_t stream_id,
                                uint64_t datalen,
                                void *conn_user_data,
                                void *stream_user_data)
{
    ConnCtx   *cc = conn_user_data;
    StreamCtx *sc = stream_user_data;

    Ns_Log(Notice, "H3[%lld] on_acked_stream_data %llu bytes cc %p sc %p",
           (long long)stream_id, (unsigned long long)datalen,
           (void*) cc, (void*)sc);

    sc->flow_blocked = NS_FALSE;
    nghttp3_conn_resume_stream(cc->h3conn, h3_stream_id(sc));
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * on_stream_close --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_stream_close'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Invoked when a stream is fully closed (either normally or due to
 *      an application-level error).  Logs the closure and unregisters
 *      the associated StreamCtx to release per-stream resources.
 *
 *----------------------------------------------------------------------
 */
static int on_stream_close(nghttp3_conn *UNUSED(conn),
                           int64_t stream_id,
                           uint64_t app_error_code,
                           void *UNUSED(conn_user_data),
                           void *stream_user_data)
{
    StreamCtx *sc = stream_user_data;

    Ns_Log(Notice, "H3[%lld] on_stream_close (app_error_code=%llu)",
           (long long)stream_id,
           (unsigned long long)app_error_code);

    /* Unregister and free our per‑stream context */
    StreamCtxUnregister(sc);

    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * on_deferred_consume --
 *
 *      Handler for the nghttp3 callback of type 'nghttp3_deferred_consume'
 *      (see https://nghttp2.org/nghttp3/types.html#c.nghttp3_callbacks).
 *      Called when nghttp3 notifies that previously deferred stream data
 *      has been consumed by the remote peer.  Trims the corresponding
 *      number of bytes from the stream’s pending transmit buffer to
 *      release flow-control credit and maintain accurate accounting.
 *
 *----------------------------------------------------------------------
 */
static int on_deferred_consume(nghttp3_conn *UNUSED(conn),
                               int64_t stream_id,
                               size_t consumed,
                               void *UNUSED(conn_user_data),
                               void *stream_user_data)
{
    StreamCtx *sc = stream_user_data;
    size_t     actual_consumed;

    Ns_Log(Notice, "H3[%lld] on_deferred_consume: consumed=%zu sc %p", (long long)stream_id, consumed, (void*)sc);

    if (sc == NULL || consumed == 0) {
        Ns_Log(Notice, "H3[%lld] on_deferred_consume: aborting, no stream context", (long long)stream_id);
        return 0;
    }

    actual_consumed = SharedTrimPending(&sc->sh, consumed, /*drain*/NS_TRUE);

    if (actual_consumed != consumed) {
        Ns_Log(Warning, "H3[%lld] consumed %zu bytes from %zu available",
               (long long)stream_id, consumed, actual_consumed);
    }

    return 0;
}



/*======================================================================
 * Function Implementations: Connection Context (ConnCtx) Lifecycle
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * ConnCtxNew --
 *
 *      Allocate and initialize a new HTTP/3 connection context (ConnCtx)
 *      for the given OpenSSL QUIC connection.  This function sets up the
 *      per-connection stream hash table, initializes synchronization
 *      primitives, and prepares the shared state used for coordinating
 *      stream I/O and wakeups within the connection.
 *
 * Results:
 *      Returns a pointer to the newly allocated ConnCtx on success.
 *      Returns NULL if memory allocation fails (in which case the
 *      provided SSL connection is freed).
 *
 * Side effects:
 *      Initializes internal synchronization (mutex), the Tcl_HashTable
 *      for active streams, and the shared wake mechanism via
 *      SharedStateInit().  Takes ownership of the SSL* handle.
 *
 *----------------------------------------------------------------------
 */
static ConnCtx*
ConnCtxNew(NsTLSConfig *dc, SSL *conn)
{
    ConnCtx *cc = ns_calloc(1, sizeof(*cc));

    if (cc == NULL) {
        SSL_free(conn);
        return NULL;
    }

    Tcl_InitHashTable(&cc->streams, TCL_ONE_WORD_KEYS);
    Ns_MutexInit(&cc->lock);
    NS_TA_INIT(cc, affinity, "ConnCtx");
    NS_TA_HANDOFF(cc, affinity, "ConnCtx");

    /* initialize shared state for this connection */
    SharedStateInit(&cc->shared, (SharedWakeFn)h3_conn_wake, dc /* or something else */);

    cc->dc = dc;
    cc->h3ssl.conn = conn;
    cc->h3ssl.bidi_sid = (uint64_t)-1;
    cc->pidx = (size_t)-1;

    return cc;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnCtxFree --
 *
 *      Release resources held by a connection context (ConnCtx), including
 *      its stream hash table and shared state used for HTTP/3 coordination.
 *      This function does not free the ConnCtx structure itself - ownership
 *      of the memory remains with the caller.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Deletes the Tcl_HashTable of active streams and destroys the
 *      SharedState associated with the connection.  Logs the teardown
 *      event for diagnostic purposes.
 *
 *----------------------------------------------------------------------
 */
static void
ConnCtxFree(ConnCtx *cc)
{
    Ns_Log(Notice, "[%lld] H3 ConnCtxFree for cc %p", (long long)cc->dc->iter, (void*)cc);

    Tcl_DeleteHashTable(&cc->streams);
    SharedStateDestroy(&cc->shared);
}

/*======================================================================
 * Function Implementations: Stream Context (StreamCtx) Management
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxInit --
 *
 *      Initialize a StreamCtx structure to a clean default state for use
 *      within an HTTP/3 connection.  This prepares mutexes, response
 *      header storage, and transmission queues, ensuring that all fields
 *      are zeroed and ready for stream lifecycle setup.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Clears the entire StreamCtx structure, initializes the internal
 *      Tcl_DString for response header storage, sets the transmission
 *      state to TX_IDLE, and initializes the per-stream mutex and
 *      transmission queues.
 *
 *----------------------------------------------------------------------
 */
static void StreamCtxInit(StreamCtx *sc) {
    memset(sc, 0, sizeof(*sc));
    Tcl_DStringInit(&sc->resp_nv_store);
    sc->resp_nv       = NULL;
    sc->resp_nvlen    = 0;
    sc->tx_state = TX_IDLE;
    sc->pidx = (size_t)-1;
    Ns_MutexInit(&sc->lock);

    // Initialize queues
    memset(&sc->tx_queued, 0, sizeof(sc->tx_queued));
    memset(&sc->tx_pending, 0, sizeof(sc->tx_pending));
}


/*
 *----------------------------------------------------------------------
 *
 * StreamCtxFree --
 *
 *      Release all resources held by a StreamCtx, including dynamically
 *      allocated request/response fields, receive buffers, and transmission
 *      queues.  This function fully destroys the per-stream state and frees
 *      the StreamCtx structure itself.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Frees method, path, authority, and scheme strings.
 *      - Frees the response header buffer and nghttp3 name/value arrays.
 *      - Frees any allocated receive-side buffer (rx_hold).
 *      - Clears and frees queued and pending chunk queues.
 *      - Destroys the associated SharedStream state.
 *      - Logs diagnostic information about the teardown.
 *      - The StreamCtx memory (sc) is deallocated at the end.
 *
 *----------------------------------------------------------------------
 */
static void StreamCtxFree(StreamCtx *sc)
{
    Ns_Log(Notice, "H3[%lld] StreamCtxFree %p %s hold buffer %p"
           " tx_queued.unread %ld tx_pending.unread %ld"
           " tx_queued.drained %ld tx_pending.drained %ld",
           (long long)sc->quic_sid, (void*)sc, H3StreamKind_str(sc->kind), (void*)sc->rx_hold,
           sc->tx_queued.unread, sc->tx_pending.unread,
           sc->tx_queued.drained, sc->tx_pending.drained);
    ns_free_const(sc->method);
    ns_free_const(sc->path);
    ns_free_const(sc->authority);
    ns_free_const(sc->scheme);
    Tcl_DStringFree(&sc->resp_nv_store);
    if (sc->resp_nv != NULL) {
        ns_free(sc->resp_nv);
        sc->resp_nv = NULL;
    }
    if (sc->nsSock != NULL) {
        Ns_Log(Notice, "[%lld] StreamCtxFree SockRelease missing", (long long)sc->cc->dc->iter);
    }
    if (sc->rx_hold != NULL) {
        ns_free(sc->rx_hold);
    }

    ChunkQueueTrim(&sc->tx_queued, SIZE_MAX, NS_FALSE);
    ChunkQueueTrim(&sc->tx_pending, SIZE_MAX, NS_FALSE);
    SharedStreamDestroy(&sc->sh);
    ns_free(sc);
}

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxFromSock --
 *
 *      Resolve and return the StreamCtx associated with the given Ns_Sock,
 *      if the socket belongs to an HTTP/3 (QUIC) stream.  This provides
 *      an O(1) lookup path using the QuicSockCtx stored in sock->arg.
 *
 * Results:
 *      Returns a pointer to the associated StreamCtx, or NULL if the
 *      socket is not tied to an HTTP/3 stream or has no StreamCtx yet.
 *
 * Side effects:
 *      - Emits diagnostic log messages describing the lookup result.
 *      - Performs non-null assertions on both dc and sock.
 *
 *----------------------------------------------------------------------
 */

static inline StreamCtx *
StreamCtxFromSock(NsTLSConfig *dc, Ns_Sock *sock)
{
    NS_NONNULL_ASSERT(dc != NULL);
    NS_NONNULL_ASSERT(sock != NULL);

    /*Ns_Log(Notice, "StreamCtxFromSock sock %p dc %p arg %p",
      (void*)sock, (void*)dc, (void*)sock->arg);*/
    if (sock->arg != NULL) {
        QuicSockCtx *qctx = (QuicSockCtx *)sock->arg;

        Ns_Log(Notice, "StreamCtxFromSock sock %p -> qctx %p sc %p",
               (void*)sock, (void*)qctx, (void*)qctx->sc);

        if (qctx != NULL && qctx->is_h3 && qctx->sc != NULL) {
            return qctx->sc;                   /* O(1) fast path */
        }
    }
    Ns_Log(Notice, "StreamCtxFromSock sock %p -> sc %p",
           (void*)sock, (void*)NULL);

    return NULL;
}
/*
 *----------------------------------------------------------------------
 *
 * StreamCtxLookup --
 *
 *      Look up or create a StreamCtx entry in the given hash table by
 *      stream ID.  The QUIC or HTTP/3 stream ID is converted into a
 *      pointer key compatible with Tcl’s one-word hash tables.
 *
 * Results:
 *      Returns a pointer to the Tcl_HashEntry corresponding to the
 *      given stream ID, or NULL if not found and create == 0.
 *
 * Side effects:
 *      If 'create' is nonzero and no entry exists, a new hash entry is
 *      created and inserted into the table.
 *
 *----------------------------------------------------------------------
 */
static Tcl_HashEntry *
StreamCtxLookup(Tcl_HashTable *ht, int64_t sid, int create) {
    int newEntry;
    const char *k = LONG2PTR(sid);
    return create
        ? Tcl_CreateHashEntry(ht, k, &newEntry)
        : Tcl_FindHashEntry(ht,  k);
}

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxGet --
 *
 *      Retrieve the StreamCtx associated with a given QUIC or HTTP/3
 *      stream ID from the connection’s stream hash table.  Optionally
 *      creates and initializes a new StreamCtx if it does not yet exist.
 *
 * Results:
 *      Returns a pointer to the StreamCtx for the given stream ID, or
 *      NULL if the stream does not exist and 'create' is false.
 *
 * Side effects:
 *      - When 'create' is nonzero and the entry does not exist, a new
 *        StreamCtx is allocated, initialized (StreamCtxInit), and linked
 *        to the connection’s shared state via SharedStreamInit.
 *      - Inserts the StreamCtx into the connection’s stream hash table.
 *      - Logs an error if called with an invalid (negative) stream ID.
 *
 *----------------------------------------------------------------------
 */
static StreamCtx *
StreamCtxGet(ConnCtx *cc, int64_t sid, int create) {
    Tcl_HashEntry *e;
    StreamCtx     *sc = NULL;

    if (sid < 0) {
        Ns_Log(Error, "H3: StreamCtxGet called with invalid stream ID: %lld", (long long)sid);
    } else {
        e = StreamCtxLookup(&cc->streams, sid, create);
        if (e != NULL) {
            sc = (StreamCtx *)Tcl_GetHashValue(e);
            if (sc == NULL && create) {
                sc = ns_calloc(1, sizeof(*sc));
                if (sc != NULL) {
                    StreamCtxInit(sc);
                    SharedStreamInit(&sc->sh, &cc->shared, sid);
                    Tcl_SetHashValue(e, sc);
                }
            }
        }
    }
    return sc;
}

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxRegister --
 *
 *      Create or retrieve a StreamCtx for the given QUIC stream ID and
 *      associate it with its SSL stream, connection context, and type
 *      (control, QPACK, bidi request, etc.).  For bidi request streams,
 *      this function also creates and attaches a new NsSock object via
 *      NsSockAccept() to integrate the stream into NaviServer’s request
 *      handling pipeline.
 *
 * Results:
 *      Returns a pointer to the initialized StreamCtx structure.
 *
 * Side effects:
 *      - Allocates and initializes a StreamCtx if it did not exist yet.
 *      - For H3_KIND_BIDI_REQ:
 *          * Creates a new NsSock and associates it with the stream.
 *          * Updates the QUIC connection’s client bidi credit limit.
 *      - Updates stream metadata such as SSL pointer, stream kind,
 *        writability flags, and back-references to the connection context.
 *      - Logs acceptance and stream registration details.
 *
 *----------------------------------------------------------------------
 */
static StreamCtx *
StreamCtxRegister(ConnCtx *cc, SSL *s, uint64_t sid, H3StreamKind kind)
{
    StreamCtx *sc = StreamCtxGet(cc, (int64_t)sid, 1 /*create*/);

    sc->ssl         = s;
    sc->cc          = cc;
    sc->quic_sid    = sid;
    sc->kind        = kind;
    sc->nsSock      = NULL;

    //Ns_Log(Notice, "H3(%lld) StreamCtxRegister (stream %p cc %p %s) -> sc %p",
    //       sid, (void*)s, (void*)cc, H3StreamKind_str(kind), (void*)sc);

    switch (kind) {
    case H3_KIND_BIDI_REQ:
        {
            Ns_Driver   *drvPtr = cc->dc->driver;
            Ns_Time      now;
            QuicSockCtx *qctx;
            char         buffer[NS_IPADDR_SIZE];

            Ns_GetTime(&now);
            sc->writable = NS_TRUE;
            /*
             * Get a fresh NsSock into sc->nsSock. Release happens via StreamCtxFree().
             */
            NsSockAccept(drvPtr, SSL_get_fd(s), (Ns_Sock**)&sc->nsSock, &now, s);

            (void)ns_inet_ntop((const struct sockaddr *)&sc->nsSock->sa, buffer, NS_IPADDR_SIZE);
            Ns_Log(Notice, "[%lld] H3 STREAM accept SockAccept returns sockPtr %p IP %s",
                   (long long)sc->cc->dc->iter, (void*)sc->nsSock, buffer);

            qctx = (QuicSockCtx *)sc->nsSock->arg;
            qctx->sc = sc;
            //qctx->ssl = s;
            h3_conn_maybe_raise_client_bidi_credit(cc, sid);
            //Ns_Log(Notice, "[%lld] H3 BIDI register can associate sock %p with sc %p", (long long)cc->dc->iter, (void*)sc->nsSock, (void*)sc);
            break;
        }
    case H3_KIND_CTRL:
    case H3_KIND_QPACK_ENCODER:
    case H3_KIND_QPACK_DECODER:
        sc->writable = NS_TRUE;
        break;
    case H3_KIND_CLIENT_UNI:
    case H3_KIND_UNKNOWN:
        sc->writable = NS_FALSE; // all client uni + our QPACK decoder
        break;
    }
    return sc;
}

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxUnregister --
 *
 *      Remove a StreamCtx from its parent connection’s stream hash table
 *      and log the unregistration.  This is typically called when a QUIC
 *      or HTTP/3 stream is closed or reset and its associated resources
 *      should no longer be tracked.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Deletes the Tcl_HashEntry for the stream ID from the connection’s
 *        stream table, if present.
 *      - Leaves the StreamCtx memory itself intact; freeing is performed
 *        separately by StreamCtxFree().
 *      - Emits a diagnostic log message showing stream IDs and SSL pointers.
 *
 *----------------------------------------------------------------------
 */
static void StreamCtxUnregister(StreamCtx *sc)
{
    ConnCtx       *cc;
    Tcl_HashEntry *e;

    NS_NONNULL_ASSERT(sc != NULL);

    cc = sc->cc;
    e = StreamCtxLookup(&cc->streams, (int64_t)sc->quic_sid, 0);

    Ns_Log(Notice, "[%lld] StreamCtxUnregister sc %p ssl %p quic_sid %lld h3_sid %lld",
           (long long)cc->dc->iter, (void*)sc, (void*)sc->ssl, (long long)sc->quic_sid, (long long)sc->h3_sid);
    /* Unregister and free our per‑stream context */
    if (e != NULL) {
        Tcl_DeleteHashEntry(e);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxRequireRxBuffer --
 *
 *      Ensure that the StreamCtx has an allocated receive buffer for
 *      reading incoming QUIC data.  If no buffer exists, this function
 *      allocates one with a default capacity and resets related
 *      receive-side state fields.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Allocates sc->rx_hold if it was NULL.
 *      - Initializes rx_cap, rx_len, rx_off, and rx_fin_pending.
 *      - May allocate up to 16 KB of memory (default buffer size).
 *
 *----------------------------------------------------------------------
 */
static void StreamCtxRequireRxBuffer(StreamCtx *sc)
{
    if (sc->rx_hold == NULL) {
        sc->rx_cap = 16384;                    /* maybe configurable ? */
        sc->rx_hold = ns_malloc(sc->rx_cap);
        sc->rx_len = sc->rx_off = 0;
        sc->rx_fin_pending = NS_FALSE;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxClaimDispatch --
 *
 *      Atomically mark a StreamCtx as having been dispatched for
 *      request handling, preventing duplicate dispatch attempts.
 *      Used to ensure that each HTTP/3 request stream is handed
 *      off to NaviServer’s request processor exactly once.
 *
 * Results:
 *      Returns NS_TRUE if this call successfully marked the stream
 *      as dispatched, or NS_FALSE if it had already been dispatched.
 *
 * Side effects:
 *      - Sets the H3_IO_REQ_DISPATCHED flag in sc->io_state when not
 *        previously set.
 *
 *----------------------------------------------------------------------
 */
static inline bool
StreamCtxClaimDispatch(StreamCtx *sc)
{
    if ((sc->io_state & H3_IO_REQ_DISPATCHED) != 0) {
        return NS_FALSE;                 /* already dispatched */
    }
    sc->io_state |= H3_IO_REQ_DISPATCHED;
    return NS_TRUE;                      /* we claimed it now */
}

/*
 *----------------------------------------------------------------------
 *
 * StreamCtxIsServerUni / StreamCtxIsClientUni / StreamCtxIsBidi --
 *
 *      Convenience predicates identifying the type of HTTP/3 stream
 *      represented by a StreamCtx.  These helpers classify a stream as
 *      one of the following:
 *
 *          - Server-initiated unidirectional streams (control, QPACK encoder/decoder)
 *          - Client-initiated unidirectional streams
 *          - Bidirectional request/response streams
 *
 * Results:
 *      Each function returns NS_TRUE if the StreamCtx matches the
 *      corresponding type, or NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline bool StreamCtxIsServerUni(const StreamCtx *sc)
{
    return (sc->kind == H3_KIND_CTRL
            || sc->kind == H3_KIND_QPACK_ENCODER
            || sc->kind == H3_KIND_QPACK_DECODER);
}

static inline bool StreamCtxIsClientUni(const StreamCtx *sc)
{
    return (sc->kind == H3_KIND_CLIENT_UNI);
}

static inline bool StreamCtxIsBidi(const StreamCtx *sc)
{
    return (sc->kind == H3_KIND_BIDI_REQ);
}

/*======================================================================
 * Function Implementations: Pollset and Event Handling Utilities
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * PollsetInit --
 *
 *      Initialize the connection-level pollset data structures for an
 *      HTTP/3 (QUIC) listener. This prepares the dynamic lists used to
 *      track active connections and associated SSL poll items, and
 *      allocates the initial poll array used by the QUIC event loop.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Allocates memory for dc->u.h3.poll_items.
 *      - Initializes list heads (conns, ssl_items) and poll capacity.
 *      - Resets the first_dead slot marker.
 *
 *----------------------------------------------------------------------
 */
static void
PollsetInit(NsTLSConfig *dc)
{
    NS_NONNULL_ASSERT(dc != NULL);

    Ns_DListInit(&dc->u.h3.conns);
    Ns_DListInit(&dc->u.h3.ssl_items);

    dc->u.h3.poll_capacity = Ns_DListCapacity(&dc->u.h3.ssl_items);
    dc->u.h3.poll_items = ns_malloc(dc->u.h3.poll_capacity * sizeof(SSL_POLL_ITEM));

    /* slot 0 placeholders */
    dc->u.h3.first_dead = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetFree --
 *
 *      Release all resources allocated for the HTTP/3 (QUIC) pollset
 *      associated with the given TLS configuration. This includes
 *      freeing the dynamically allocated poll item array and clearing
 *      the connection and SSL item lists.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Frees dc->u.h3.poll_items and resets its pointer and capacity.
 *      - Empties and deallocates the DLists for active connections and
 *        SSL items.
 *
 *----------------------------------------------------------------------
 */
static void
PollsetFree(NsTLSConfig *dc)
{
    if (dc->u.h3.poll_items) {
        ns_free(dc->u.h3.poll_items);
        dc->u.h3.poll_items = NULL;
    }
    dc->u.h3.poll_capacity = 0;
    Ns_DListFree(&dc->u.h3.ssl_items);
    Ns_DListFree(&dc->u.h3.conns);
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetEnsurePollCapacity --
 *
 *      Ensure that the poll item array (dc->u.h3.poll_items) has
 *      sufficient capacity to cover all SSL items currently tracked
 *      in the HTTP/3 (QUIC) pollset. If the required capacity exceeds
 *      the existing one, the array is reallocated and the newly added
 *      section is zero-initialized.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - May reallocate dc->u.h3.poll_items to a larger buffer.
 *      - Updates dc->u.h3.poll_capacity accordingly.
 *      - Zeroes the newly allocated portion to maintain clean state.
 *
 *----------------------------------------------------------------------
 */
static inline void
PollsetEnsurePollCapacity(NsTLSConfig *dc)
{
    size_t need = Ns_DListCapacity(&dc->u.h3.ssl_items);
    if (need > dc->u.h3.poll_capacity) {
        dc->u.h3.poll_items = ns_realloc(dc->u.h3.poll_items, need * sizeof(SSL_POLL_ITEM));
        /* Zero-initialize the newly added tail, if any */
        memset(&dc->u.h3.poll_items[dc->u.h3.poll_capacity], 0,
               (need - dc->u.h3.poll_capacity) * sizeof(SSL_POLL_ITEM));
        dc->u.h3.poll_capacity = need;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetCount --
 *
 *      Return the number of active entries currently in the HTTP/3
 *      (QUIC) pollset. The pollset uses npoll as an index of the last
 *      valid slot, so the effective count is npoll + 1. If npoll is
 *      uninitialized (set to -1), the count is zero.
 *
 * Results:
 *      The current number of active poll items.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline size_t
PollsetCount(const NsTLSConfig *dc)
{
    return (dc->u.h3.npoll == (size_t)-1) ? 0 : dc->u.h3.npoll+1;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetPrint --
 *
 *      Diagnostic utility to log the current contents and layout of the
 *      HTTP/3 pollset. Each poll entry (connection or stream) is printed
 *      with its index, pointer, and an asterisk (*) if it is within the
 *      active npoll range.
 *
 *      When the 'skip' flag is true, consecutive empty stream slots are
 *      compressed in the output to reduce verbosity.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Produces verbose log output describing the current pollset
 *      entries and capacity for debugging and inspection.
 *
 *----------------------------------------------------------------------
 */
static void
PollsetPrint(NsTLSConfig *dc, const char *prefix, bool skip)
{
    bool last_stream_empty = NS_FALSE;
    Ns_Log(Notice, "Pollset size %ld capacity %ld", dc->u.h3.npoll + 1, dc->u.h3.poll_capacity);

    for (size_t idx = 0; idx < dc->u.h3.poll_capacity; idx++) {
        if (!(skip && last_stream_empty)) {
            Ns_Log(Notice, "   %s poll [%ld] %c s %p", prefix, idx,
                   idx <= dc->u.h3.npoll ? '*' : ' ',
                   (void*)(dc->u.h3.ssl_items.data[idx]));
        }
        last_stream_empty = (dc->u.h3.ssl_items.data[idx] == NULL);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * PollsetDefaultConnErrors / PollsetDefaultStreamErrors --
 *
 *      Utility functions to augment a given event mask with default
 *      error and exception events for QUIC connections or streams.
 *
 *      - PollsetDefaultConnErrors() adds connection-level error bits
 *        (EC, ECD, ER, EW) to the supplied event mask.
 *      - PollsetDefaultStreamErrors() adds per-stream error bits
 *        (ER, EW) to the supplied event mask.
 *
 * Results:
 *      Returns the input event mask with additional default error bits set.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline uint64_t PollsetDefaultConnErrors(uint64_t ev) {
    return ev | SSL_POLL_EVENT_EC | SSL_POLL_EVENT_ECD
        | SSL_POLL_EVENT_ER | SSL_POLL_EVENT_EW;
}
static inline uint64_t PollsetDefaultStreamErrors(uint64_t ev) {
    return ev | SSL_POLL_EVENT_ER | SSL_POLL_EVENT_EW;
}


/*
 *----------------------------------------------------------------------
 *
 * PollsetAdd --
 *
 *      Add a single SSL* object (representing either a QUIC connection
 *      or a stream) to the HTTP/3 pollset with a caller-provided event
 *      mask and optional masking function.
 *
 *      This function assigns the SSL’s poll descriptor, applies the
 *      specified event mask (or calls maskf() to adjust it), and logs
 *      diagnostic details such as stream kind and mask bits if a label
 *      is provided.
 *
 * Results:
 *      Returns the index of the newly added pollset entry.
 *
 * Side effects:
 *      - Extends dc->u.h3.ssl_items and dc->u.h3.poll_items if capacity
 *        is insufficient.
 *      - Updates dc->u.h3.npoll.
 *      - Emits a diagnostic log entry when 'label' is non-NULL.
 *
 *----------------------------------------------------------------------
 */
static size_t PollsetAdd(NsTLSConfig *dc, SSL *s, uint64_t events, PollsetMaskProc maskf,
                         const char *label, H3StreamKind kind)
{
    size_t idx;

    NS_NONNULL_ASSERT(dc != NULL);
    NS_NONNULL_ASSERT(s  != NULL);

    idx = ++dc->u.h3.npoll;

    if (dc->u.h3.ssl_items.size <= dc->u.h3.npoll) {
        /* Append in lockstep */
        Ns_DListAppend(&dc->u.h3.ssl_items, s); /* may double capacity */
        PollsetEnsurePollCapacity(dc);     /* grow poll_items if needed */
    } else {
        dc->u.h3.ssl_items.data[idx] = s;
    }

    dc->u.h3.poll_items[idx].desc   = SSL_as_poll_descriptor(s);
    dc->u.h3.poll_items[idx].events = maskf ? maskf(events) : events;

    if (label != NULL) {
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        Ns_Log(Notice, "[%lld] H3[%lld] %s %p %s mask %s",
               (long long)dc->iter, (long long)SSL_get_stream_id(s),
               label, (void*)s,
               kind == H3_KIND_UNKNOWN ? "conn" : H3StreamKind_str(kind),
               DStringAppendSslPollEventFlags(&ds, dc->u.h3.poll_items[idx].events)
               );
        Tcl_DStringFree(&ds);
    }
    return idx;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetAddConnection --
 *
 *      Add a QUIC connection (SSL*) to the HTTP/3 pollset using the
 *      default connection-level error mask. This ensures the connection
 *      is tracked for all relevant events (I/O and error conditions)
 *      within the main poll loop.
 *
 *      The function also links the corresponding ConnCtx to the
 *      active-connection list and stores its pollset index for later
 *      lookup.
 *
 * Results:
 *      Returns the pollset index of the newly added connection entry.
 *
 * Side effects:
 *      - Extends the pollset and the active connection list.
 *      - Records the poll index in the ConnCtx (cc->pidx).
 *      - Registers default connection error bits (EC, ECD, ER, EW).
 *
 *----------------------------------------------------------------------
 */
static inline size_t PollsetAddConnection(NsTLSConfig *dc, SSL *conn, uint64_t events) {
    ConnCtx *cc = SSL_get_ex_data(conn, dc->u.h3.cc_idx);
    size_t   idx = PollsetAdd(dc, conn, events, PollsetDefaultConnErrors, "PollsetAddConnection", H3_KIND_UNKNOWN);

    Ns_DListAddUnique(&dc->u.h3.conns, cc);
    cc->pidx = idx;
    //Ns_Log(Notice, "[%lld] H3 connection added on idx %ld", (long long)dc->iter, idx);
    return idx;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetAddStream --
 *
 *      Add a single QUIC stream (SSL*) to the HTTP/3 pollset using the
 *      default per-stream error mask. This function ensures that the
 *      stream is monitored for read/write and error events during the
 *      main polling cycle.
 *
 *      The associated StreamCtx is resolved via SSL_get_ex_data() and
 *      its pollset index (pidx) is updated for fast lookup and cleanup.
 *
 * Results:
 *      Returns the pollset index assigned to the new stream entry.
 *
 * Side effects:
 *      - Extends the pollset if additional capacity is required.
 *      - Records the poll index in the associated StreamCtx.
 *      - Registers default stream error bits (ER, EW).
 *
 *----------------------------------------------------------------------
 */

static inline size_t PollsetAddStream(NsTLSConfig *dc, SSL *stream, uint64_t events, H3StreamKind kind) {
    StreamCtx *sc = SSL_get_ex_data(stream, dc->u.h3.sc_idx);
    size_t     idx = PollsetAdd(dc, stream, events, PollsetDefaultStreamErrors, "PollsetAddStream", kind);

    sc->pidx = idx;
    //Ns_Log(Notice, "[%lld] H3 stream added on idx %ld", (long long)dc->iter, idx);
    return idx;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetAddStreamRegister --
 *
 *      Create, initialize, and register a new HTTP/3 stream (SSL*) within
 *      the pollset and its corresponding StreamCtx. This function performs
 *      all necessary setup for QUIC stream lifecycle tracking and event
 *      polling.
 *
 *      Specifically, it:
 *        - Sets the stream to nonblocking mode
 *        - Associates the app data (NsTLSConfig) and ex_data (ConnCtx, StreamCtx)
 *        - Allocates and registers a new StreamCtx for the stream
 *        - Determines the appropriate poll event mask based on stream kind
 *        - Adds the stream to the active pollset for monitoring
 *
 * Results:
 *      Returns a pointer to the initialized StreamCtx on success,
 *      or NULL if stream registration or setup fails.
 *
 * Side effects:
 *      - Modifies SSL ex_data to attach StreamCtx and connection context.
 *      - Extends the pollset with a new entry for the stream.
 *      - May log errors or warnings on invalid stream IDs or setup failures.
 *
 *----------------------------------------------------------------------
 */
static StreamCtx *
PollsetAddStreamRegister(ConnCtx *cc, SSL *s, H3StreamKind kind)
{
    StreamCtx   *sc;
    NsTLSConfig *dc = cc->dc;
    uint64_t     mask = SSL_POLL_EVENT_ER | SSL_POLL_EVENT_EW;
    uint64_t     sid  = SSL_get_stream_id(s);

    if (sid == (uint64_t)-1) {
        Ns_Log(Error, "PollsetAddStreamRegister: no stream id for kind %s",
               H3StreamKind_str(kind));
        return NULL;
    }

    OSSL_TRY(SSL_set_blocking_mode(s, 0));
    SSL_set_app_data(s, dc);
    //SSL_set_ex_data(s, dc->u.h3.cc_idx, cc);

    sc = StreamCtxRegister(cc, s, sid, kind);
    if (sc == NULL) {
        Ns_Log(Error, "PollsetAddStreamRegister: cannot register stream context for %s",
               H3StreamKind_str(kind));
        return NULL;
    }
    SSL_set_ex_data(s, dc->u.h3.sc_idx, sc);

    switch (kind) {
    case H3_KIND_CTRL:
    case H3_KIND_QPACK_ENCODER:
    case H3_KIND_QPACK_DECODER:
        /* Server-created uni streams are write-only from the server point of view */
        mask |= SSL_POLL_EVENT_W;
        break;

    case H3_KIND_CLIENT_UNI:
        /* Client-created uni streams are read-only from the server point of view */
        mask |= SSL_POLL_EVENT_R;
        break;

    case H3_KIND_BIDI_REQ:
        /* Client bidi request: we’ll read request; add W later when we have a response */
        mask |= SSL_POLL_EVENT_R;
        break;

    case H3_KIND_UNKNOWN:
    default:
        /* Be conservative: read only until we know what it is */
        mask |= SSL_POLL_EVENT_R;
    }

    /* this should be the only place, where this function is called */
    PollsetAddStream(dc, s, mask, kind);

    return sc;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetGetSlot --
 *
 *      Determine the pollset index (slot) associated with a given QUIC
 *      stream or connection. The function first attempts to return the
 *      cached index stored in the StreamCtx (pidx) for O(1) lookup.
 *      If the cached index is invalid or stale, it performs a linear
 *      search through the pollset to find the matching SSL pointer.
 *
 * Results:
 *      Returns the pollset index (0..npoll) if found, or (size_t)-1 if
 *      the stream is not currently present in the pollset.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline size_t
PollsetGetSlot(NsTLSConfig *dc, SSL *s, const StreamCtx *sc)
{
    //NS_NONNULL_ASSERT(sc != NULL);

    if (sc != NULL && sc->pidx != (size_t)-1 && dc->u.h3.ssl_items.data[sc->pidx] == (void*)s) {
        return sc->pidx;

    } else {
        /* fallback: get via search */
        size_t i;

        for (i = 0; i <= dc->u.h3.npoll; ++i) {
            if ((SSL *)dc->u.h3.ssl_items.data[i] == s) {
                return i;
            }
        }
    }
    return (size_t)-1;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetGetEvents / PollsetSetEvents --
 *
 *      Utility accessors for retrieving and updating the active event mask
 *      of a given SSL stream or connection within the HTTP/3 pollset.
 *
 *      PollsetGetEvents() returns the currently registered event flags
 *      (e.g., read/write/error bits) for the specified SSL handle.
 *
 *      PollsetSetEvents() updates the event mask for the same, allowing
 *      dynamic reconfiguration of what conditions the poll loop monitors.
 *      Both functions use PollsetGetSlot() to locate the stream’s index
 *      in the pollset, falling back gracefully if not found.
 *
 * Results:
 *      PollsetGetEvents() -> 64-bit event mask or 0 if not found.
 *      PollsetSetEvents() -> None.
 *
 * Side effects:
 *      PollsetSetEvents() mutates the stored event mask for the stream.
 *
 *----------------------------------------------------------------------
 */
static inline uint64_t
PollsetGetEvents(NsTLSConfig *dc, SSL *s, const StreamCtx *sc)
{
    size_t idx = PollsetGetSlot(dc, s, sc);
    if (idx != (size_t)-1) {
        return dc->u.h3.poll_items[idx].events;
    }
    return 0;
}

static inline void
PollsetSetEvents(NsTLSConfig *dc, SSL *s, const StreamCtx *sc, uint64_t events)
{
    size_t idx = PollsetGetSlot(dc, s, sc);
    if (idx != (size_t)-1) {
        dc->u.h3.poll_items[idx].events = events;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetUpdateEvents --
 *
 *      Atomically modify the poll event mask for a given QUIC connection
 *      or stream within the HTTP/3 pollset. This helper sets and clears
 *      event bits in-place, while always ensuring the appropriate error
 *      mask (H3_STREAM_ERR_MASK or H3_CONN_ERR_MASK) remains active.
 *
 *      This is typically used when dynamically adjusting interest in
 *      read/write readiness during QUIC or nghttp3 processing.
 *
 * Results:
 *      Returns the new effective 64-bit event mask after modification.
 *      Returns 0 if the stream or connection was not found in the pollset.
 *
 * Side effects:
 *      Updates the stored event mask in dc->u.h3.poll_items[idx].
 *      Logs may reflect resulting event transitions.
 *
 *----------------------------------------------------------------------
 */
static inline uint64_t
PollsetUpdateEvents(NsTLSConfig *dc, SSL *s, const StreamCtx *sc,
                    uint64_t set_bits, uint64_t clear_bits)
{
    const uint64_t errmask = sc != NULL ? H3_STREAM_ERR_MASK : H3_CONN_ERR_MASK;
    const size_t   idx     = PollsetGetSlot(dc, s, sc);

    if (idx == (size_t)-1) {
        Ns_Log(Warning, "PollsetUpdateEvents: item not found (sc=%p, ssl=%p)",
               (const void*)sc, (void*)s);
        return 0;
    } else {
        const uint64_t m = dc->u.h3.poll_items[idx].events;
        const uint64_t desired = ((m | errmask | set_bits) & ~clear_bits);

        if (desired != m) {
            dc->u.h3.poll_items[idx].events = desired;
        }
        return desired;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetEnableRead / PollsetDisableRead /
 * PollsetEnableWrite / PollsetDisableWrite --
 *
 *      Convenience wrappers around PollsetUpdateEvents() to toggle
 *      interest in read (R) and write (W) readiness for a given QUIC
 *      connection or HTTP/3 stream within the pollset.
 *
 *      These helpers make it easy for higher-level HTTP/3 logic
 *      (e.g., stream state machines, nghttp3 callbacks, or response
 *      writers) to dynamically adjust I/O event subscriptions without
 *      needing to manipulate bitmasks directly.
 *
 *      Each function logs the transition with contextual information
 *      (iteration counter, stream ID, stream kind, and a label).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies the event mask in dc->u.h3.poll_items to enable or
 *      disable read/write readiness notifications for the given stream.
 *
 *----------------------------------------------------------------------
 */
static inline void PollsetEnableRead(NsTLSConfig *dc, SSL *s, StreamCtx *sc) {
    (void)PollsetUpdateEvents(dc, s, sc, SSL_POLL_EVENT_R, 0);
}
static inline void PollsetDisableRead(NsTLSConfig *dc, SSL *s, const StreamCtx *sc, const char *label) {
    Ns_Log(Notice, "[%lld] H3 PollsetDisableRead %p %s %s",
           (long long)dc->iter, (void*)s, sc != NULL?H3StreamKind_str(sc->kind):"other", label);
    (void)PollsetUpdateEvents(dc, s, sc, 0, SSL_POLL_EVENT_R);
}
static inline void PollsetEnableWrite(NsTLSConfig *dc, SSL *s, StreamCtx *sc, const char *label) {
    Ns_Log(Notice, "[%lld] H3[%ld] PollsetEnableWrite %p %s %s",
           (long long)dc->iter, sc != NULL?(long)sc->quic_sid:-1, (void*)s, sc != NULL?H3StreamKind_str(sc->kind):"other", label);
    (void)PollsetUpdateEvents(dc, s, sc, SSL_POLL_EVENT_W, 0);
}
static inline void PollsetDisableWrite(NsTLSConfig *dc, SSL *s, StreamCtx *sc, const char *label) {
    Ns_Log(Notice, "[%lld] H3[%ld] PollsetDisableWrite %p %s %s",
           (long long)dc->iter, sc != NULL?(long)sc->quic_sid:-1, (void*)s, sc != NULL?H3StreamKind_str(sc->kind):"other", label);
    (void)PollsetUpdateEvents(dc, s, sc, 0, SSL_POLL_EVENT_W);
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetUpdateConnPollInterest --
 *
 *      Update the pollset event mask for a QUIC connection (as opposed to
 *      individual HTTP/3 streams). This function ensures the connection’s
 *      error events are always monitored and dynamically enables or disables
 *      socket-level readiness notifications based on connection state.
 *
 *      Specifically:
 *        - Enables OSB/OSU (OpenSSL BIO read/write readiness) while the
 *          handshake is in progress or when write activity is pending.
 *        - Optionally clears OSB/OSU when the connection is fully idle and
 *          handshake has completed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Adjusts the connection’s event mask in the pollset via
 *      PollsetUpdateEvents().
 *      Keeps error conditions (H3_CONN_ERR_MASK) always active.
 *
 *----------------------------------------------------------------------
 */
static inline void
PollsetUpdateConnPollInterest(ConnCtx *cc)
{
    NsTLSConfig *dc = cc->dc;
    uint64_t     set_bits   = H3_CONN_ERR_MASK;
    uint64_t     clear_bits = 0u;

    /* Keep OSB/OSU ON while handshake runs or we have pending writes */
    if (!cc->handshake_done || cc->wants_write) {
        set_bits |= SSL_POLL_EVENT_OSB | SSL_POLL_EVENT_OSU;
    } else {
        /* Optional: clear when fully idle */
        clear_bits |= SSL_POLL_EVENT_OSB | SSL_POLL_EVENT_OSU;
    }

    (void)PollsetUpdateEvents(dc, cc->h3ssl.conn, /*sc=*/NULL, set_bits, clear_bits);
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetHandleListenerEvents --
 *
 *      Drive OpenSSL’s QUIC event loop for all registered listener SSL
 *      objects. This function should be called once per tick after
 *      processing poll results (so that any I/O-related events like IC,
 *      F, or EL are handled first), and also on poll timeouts to let
 *      OpenSSL perform timer-driven tasks such as Retry, loss recovery,
 *      or connection migration management.
 *
 *      Each listener socket is advanced via SSL_handle_events(), which
 *      performs nonblocking internal QUIC state machine work and may
 *      generate outbound packets.
 *
 * Results:
 *      Returns the number of listeners successfully processed by
 *      SSL_handle_events(). On error, logs diagnostic information
 *      including the OpenSSL library and reason codes.
 *
 * Side effects:
 *      - Drives internal OpenSSL QUIC timers and retransmission logic.
 *      - May generate outbound datagrams.
 *      - Emits diagnostic logs for each listener processed.
 *
 *----------------------------------------------------------------------
 */
static size_t
PollsetHandleListenerEvents(NsTLSConfig *dc)
{
    size_t i, nticked = 0;

    NS_NONNULL_ASSERT(dc != NULL);

    for (i = 0; i < dc->u.h3.nr_listeners; i++) {
        int  rc;
        SSL *ls = dc->u.h3.ssl_items.data[i];

        if (ls == NULL) {
            continue;
        }

        rc = SSL_handle_events(ls);   /* nonblocking; may generate egress */

        Ns_Log(Notice, "[%lld] SSL_handle_events in PollsetHandleListenerEvents listener %p => %d",
               (long long)dc->iter, (void*)ls, SSL_handle_events(ls));

        if (rc < 0) {
            int l = ERR_GET_LIB(ERR_peek_error());
            int r = ERR_GET_REASON(ERR_peek_error());

            Ns_Log(Error, "H3 listener %p SSL_handle_events failed lib=%d reason=%d", (void*)ls, l, r);
        } else {
            nticked++;
        }
    }
    return nticked;
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetMarkDead --
 *
 *      Remove a QUIC connection or HTTP/3 stream (identified by its SSL*)
 *      from the active pollset and mark its slot as dead. This function
 *      clears the corresponding entry in dc->u.h3.ssl_items and resets
 *      its poll event mask, ensuring that the poll loop will no longer
 *      monitor the socket.
 *
 *      The lookup is optimized via back-references:
 *        1. Try StreamCtx::pidx or ConnCtx::pidx for a fast match.
 *        2. If not found, fall back to a linear scan of the pollset.
 *
 *      Once found, the entry is nulled out, its events cleared, and
 *      dc->u.h3.first_dead updated to track the earliest free slot.
 *      For connections, the function also removes the ConnCtx from
 *      the connection list (dc->u.h3.conns).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Removes the SSL* entry from the pollset and disables its events.
 *      - Updates dc->u.h3.first_dead for potential reuse.
 *      - May modify the connection list if a ConnCtx is removed.
 *      - Produces detailed diagnostic logs, including slot index and reason.
 *
 *----------------------------------------------------------------------
 */
static void
PollsetMarkDead(ConnCtx *cc, SSL *ssl, const char *msg)
{
    NsTLSConfig *dc  = cc->dc;
    StreamCtx   *sc  = SSL_get_ex_data(ssl, dc->u.h3.sc_idx);
    size_t       idx = (size_t)-1;

    //PollsetPrint(dc, "before del", NS_FALSE);

    /* Fast path via backrefs */
    if (sc != NULL
        && sc->pidx != (size_t)-1
        && sc->pidx <= dc->u.h3.npoll
        && (SSL *)dc->u.h3.ssl_items.data[sc->pidx] == ssl) {
        idx = sc->pidx;
        //Ns_Log(Notice, "[%lld] PollsetMarkDead ssl %p: got idx %ld from cc", (long long)dc->iter, (void*)ssl, idx);
        sc->pidx = (size_t)-1;
    } else if (ssl == cc->h3ssl.conn
               && cc->pidx != (size_t)-1
               && cc->pidx <= dc->u.h3.npoll
               && (SSL *)dc->u.h3.ssl_items.data[cc->pidx] == ssl) {
        idx = cc->pidx;
        //Ns_Log(Notice, "[%lld] PollsetMarkDead ssl %p: got idx %ld from cc", (long long)dc->iter, (void*)ssl, idx);
        cc->pidx = (size_t)-1;
    }

    /* Fallback: scan for entry with the ssl item */
    if (idx == (size_t)-1) {
        Ns_Log(Notice, "[%lld] PollsetMarkDead ssl %p: scan for idx", (long long)dc->iter, (void*)ssl);
        for (size_t i = dc->u.h3.nr_listeners; i < dc->u.h3.npoll; ++i) {
            if ((SSL *)dc->u.h3.ssl_items.data[i] == ssl) {
                idx = i;
                break;
            }
        }
        if (idx == (size_t)-1) {
            Ns_Log(Notice, "[%lld] PollsetMarkDead: ssl %p not found (%s)",
                   (long long)dc->iter, (void*)ssl, msg != NULL ? msg : "");
            return;
        }
    }

    assert(idx != (size_t)-1);

    /* In case, we got a connection, remove it from the list of connections. */
    if (ssl == cc->h3ssl.conn) {
        Ns_DListDelete(&dc->u.h3.conns, cc);
    }

    if (dc->u.h3.ssl_items.data[idx] != NULL) {
        /* Punch the hole and record earliest dead slot */
        //Ns_Log(Notice, "[%lld] PollsetMarkDead ssl %p: punch hole at idx %ld", (long long)dc->iter, (void*)ssl, idx);
        dc->u.h3.ssl_items.data[idx]    = NULL;
        dc->u.h3.poll_items[idx].events = 0;
        if (dc->u.h3.first_dead == 0 || idx < dc->u.h3.first_dead) {
            dc->u.h3.first_dead = idx;
        }
        if (sc != NULL) {
            Ns_Log(Notice, "[%lld] H3[%lld] PollsetMarkDead %p %s (at slot [%zu] (%s)",
                   (long long)dc->iter, (long long)sc->quic_sid, (void *)ssl, H3StreamKind_str(sc->kind), idx, msg ? msg : "");
        } else {
            Ns_Log(Notice, "[%lld] PollsetMarkDead %p at slot [%zu] (%s)",
                   (long long)dc->iter, (void *)ssl, idx, msg ? msg : "");
        }
    } else {
        Ns_Log(Notice, "[%lld] PollsetMarkDead %p redundant call (%s)",
               (long long)dc->iter, (void *)ssl, msg ? msg : "");
    }
    //PollsetPrint(dc, "after del", NS_FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetSweep --
 *
 *      Post-loop sweeper that runs once per event-loop tick, before
 *      PollsetConsolidate(dc). It walks the current pollset and:
 *        - For connection entries: frees those that are fully shut down
 *          and have no live streams (quic_conn_can_be_freed_postloop).
 *        - For stream entries: optionally finalizes idle streams,
 *          drops R/W interest for closed halves, and reaps streams that
 *          are definitively dead (both halves FIN or RESET and queues empty).
 *
 *      To avoid invalidating the iteration state, objects selected for
 *      destruction are first collected into a small local array and are
 *      SSL_free()'d only after the scan completes. Pollset slots are
 *      punched out immediately via PollsetMarkDead(), and StreamCtx
 *      entries are unregistered once deemed dead.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Removes dead streams/connections from the pollset and connection
 *        list; updates dc->u.h3.first_dead for reuse.
 *      - May disable per-entry R/W interest based on observed close state.
 *      - Frees SSL* objects for dead streams/connections after the scan.
 *      - Produces diagnostic logs for each action (skip/postpone/kill).
 *
 *----------------------------------------------------------------------
 */
static void
PollsetSweep(NsTLSConfig *dc)
{
    /* Collect what to free after we finish iterating the arrays */
    enum { MAX_SWEEP_FREES = 256 };
    SSL   *to_free[MAX_SWEEP_FREES];
    size_t i, nfree = 0;

    Ns_Log(Notice, "[%lld] PollsetSweep begin npoll %ld", (long long)dc->iter, PollsetCount(dc));
    for (i = 0; i < PollsetCount(dc); i++) {
        int        stype;
        SSL       *s = dc->u.h3.ssl_items.data[i];
        ConnCtx   *cc;
        uint64_t   sid, mask;
        StreamCtx *sc;
        bool       finalized;

        if (s == NULL) {
            continue;
        }

        /* Resolve the owning connection context for this entry (conn or stream).
           Listener sockets won’t have one  -  skip those automatically. */

        cc = SSL_get_ex_data(s, dc->u.h3.cc_idx);
        if (cc == NULL) {
            /* listener or foreign object */
            continue;
        }

        /* 1) Connection object? Treat it as conn regardless of stream_type. */
        if (s == cc->h3ssl.conn) {
            if (quic_conn_can_be_freed_postloop(s, cc)) {
                Ns_Log(Notice, "[%lld] H3 PollsetSweep: kill conn %p", (long long)dc->iter, (void*)s);
                PollsetMarkDead(cc, s, "conn postloop free");
                if (nfree < MAX_SWEEP_FREES) {
                    to_free[nfree++] = s;
                }
            }
            continue;
        }

        /* 2) Non-stream objects (e.g., listener) */
        stype = SSL_get_stream_type(s);
        if (stype == SSL_STREAM_TYPE_NONE) {
            continue;
        }

        /* 3) Streams without a usable id yet – postpone */
        sid = SSL_get_stream_id(s);
        if (sid == (uint64_t)-1) {  /* UINT64_MAX */
            Ns_Log(Notice, "[%lld] H3 PollsetSweep: postpone unknown stream %p type %d %s",
                   (long long)dc->iter, (void*)s, stype, ossl_quic_stream_type_str(stype));
            continue;
        }

        /* From here on we have only streams with valid sids */
        sc = SSL_get_ex_data(s, dc->u.h3.sc_idx);

        if (sc == NULL) {
            Ns_Log(Notice, "[%lld] H3 PollsetSweep: stream %p sid %llu not registered yet; skip",
                   (long long)dc->iter, (void*)s, (long long)sid);
            continue;
        }

        if (!StreamCtxIsServerUni(sc)) {
            finalized = h3_stream_maybe_finalize(sc, "PollsetSweep");
            if (!finalized && !sc->seen_io && !H3_TX_CLOSED(sc) && !H3_RX_CLOSED(sc)) {
                /* We've already disabled W inside maybe_finalize if idle. */
                Ns_Log(Notice, "[%lld] H3 PollsetSweep: stream %p sid %llu already disabled W; skip",
                       (long long)dc->iter, (void*)s, (long long)sid);
                continue;
            }
        }

        /* Still keep the "don’t free without IO" rule for freeing. */
        if (!sc->seen_io && !H3_TX_CLOSED(sc) && !H3_RX_CLOSED(sc)) {
            /* We've already disabled EW above if nothing to write. */
            Ns_Log(Notice, "[%lld] H3 PollsetSweep: don't sweep stream without io %p"
                   " kind %s tx_queued.unread %ld tx_pending.unread %ld",
                   (long long)dc->iter, (void*)s,
                   H3StreamKind_str(sc->kind),
                   sc->tx_queued.unread, sc->tx_pending.unread);
            continue;
        }

        /* Adjust poll interest for closed sides */
        mask = PollsetGetEvents(dc, s, sc);

        {
            const bool rx_closed = (sc->io_state & (H3_IO_RX_FIN | H3_IO_RESET)) != 0;
            const bool tx_closed = (sc->io_state & (H3_IO_TX_FIN | H3_IO_RESET)) != 0;

            /*
             * If peer finished our read side (or stream was reset), and we
             * have no unread app bytes left, drop R interest.
             */
            if (rx_closed && (mask & SSL_POLL_EVENT_R) && sc->rx_len == sc->rx_off) {
                PollsetDisableRead(dc, s, sc, "PollsetSweep: no unread data");
                mask &= ~SSL_POLL_EVENT_R;
            }
            /*
             * If we concluded the write side (or stream was reset), drop W
             * interest.  We avoid SSL_get_stream_write_state() entirely to
             * prevent races when the send-part is already torn down inside
             * libssl.
             */
            if (StreamCtxIsServerUni(sc)) {
                if (tx_closed && (mask & SSL_POLL_EVENT_W)) {
                    PollsetDisableWrite(dc, s, sc, "PollsetSweep tx closed");
                    mask &= ~SSL_POLL_EVENT_W;
                }
            }

            /*
             * Definitely-dead streams: either RESET, or both sides FINed,
             * and all queues are empty (nothing left to deliver or flush).
             */
            if ( ((sc->io_state & H3_IO_RESET) != 0 ||
                  ((sc->io_state & H3_IO_RX_FIN) && (sc->io_state & H3_IO_TX_FIN)))
                 && sc->rx_len == sc->rx_off
                 && sc->tx_queued.unread  == 0
                 && sc->tx_pending.unread == 0 ) {
                Ns_Log(Notice, "[%lld] H3 PollsetSweep: kill stream %p kind %s "
                       "rx.buffered %d tx_queued.unread %ld tx_pending.unread %ld",
                       (long long)dc->iter, (void*)s, H3StreamKind_str(sc->kind),
                       sc->rx_len == sc->rx_off, sc->tx_queued.unread, sc->tx_pending.unread);

                PollsetMarkDead(cc, s, "sweep: stream definitely dead");
                StreamCtxUnregister(sc);
                if (nfree < MAX_SWEEP_FREES)
                    to_free[nfree++] = s;
            }
        }

    }
    /* Now it’s safe to actually free the SSL objects. */
    for (size_t k = 0; k < nfree; k++) {
        Ns_Log(Notice, "[%lld] PollsetSweep calls SSL_free %p", (long long)dc->iter, (void*)to_free[k]);
        SSL_free(to_free[k]);
    }
    Ns_Log(Notice, "[%lld] PollsetSweep DONE", (long long)dc->iter);
}

/*
 *----------------------------------------------------------------------
 *
 * PollsetConsolidate --
 *
 *      Compact the pollset by eliminating holes left behind by removed
 *      connections or streams. This function performs an in-place
 *      "swap-with-last" consolidation pass starting from the first known
 *      dead slot (dc->u.h3.first_dead) up to the current logical end
 *      (dc->u.h3.npoll).
 *
 *      For each NULL entry encountered, the last live SSL* in the pollset
 *      is moved into the hole, its corresponding poll descriptor and event
 *      mask are copied, and its back-reference (sc->pidx or cc->pidx) is
 *      updated to reflect the new index.
 *
 *      Once the last live entry has been moved, the tail slot is cleared
 *      and dc->u.h3.npoll is decremented. The process continues until all
 *      holes before the end are eliminated, leaving the array contiguous.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Mutates dc->u.h3.ssl_items and dc->u.h3.poll_items in place.
 *      - Updates per-stream and per-connection index references.
 *      - Shrinks dc->u.h3.npoll and resets dc->u.h3.first_dead to zero.
 *      - Emits detailed diagnostic logs before and after consolidation.
 *
 *----------------------------------------------------------------------
 */
static void
PollsetConsolidate(NsTLSConfig *dc)
{
    NS_NONNULL_ASSERT(dc != NULL);

    if (dc->u.h3.first_dead > 0) {
        size_t i    = dc->u.h3.first_dead;     /* destination index for the next live slot */
        size_t last = dc->u.h3.npoll;

        //PollsetPrint(dc, "before consolidate", NS_FALSE);

        while (i <= last) {
            if (dc->u.h3.ssl_items.data[i] == NULL) {
                /* Found a hole at position i, move last live entry into i */
                if (i != last) {
                    SSL *s;

                    /*
                     * swap-with-last: move last live entry down into hole.
                     */
                    dc->u.h3.ssl_items.data[i] = dc->u.h3.ssl_items.data[last];
                    dc->u.h3.poll_items[i]     = dc->u.h3.poll_items[last];
                    s = dc->u.h3.ssl_items.data[i];
                    if (s != NULL) {
                        ConnCtx   *cc = SSL_get_ex_data(s, dc->u.h3.cc_idx);
                        StreamCtx *sc = SSL_get_ex_data(s, dc->u.h3.sc_idx);

                        if (sc != NULL) {
                            /* we moved a stream */
                            sc->pidx = i;
                        } else if (cc != NULL && s == cc->h3ssl.conn) {
                            /* we moved a connection */
                            cc->pidx = i;
                        } else {
                            Ns_Log(Notice, "[%lld] Consolidate: swapped hole %zu no index update for %p",
                                   (long long)dc->iter, i, (void*)s);
                        }
                    } else {
                        //Ns_Log(Notice, "[%lld] Consolidate: swapped hole %zu with ZERO ssl", (long long)dc->iter, i);
                    }

                    Ns_Log(Notice, "[%lld] Consolidate: swapped hole %zu with slot %zu",
                           (long long)dc->iter, i, last);
                }

                /* Clear the old-last slot, which was moved */
                dc->u.h3.ssl_items.data[last]    = NULL;
                dc->u.h3.poll_items[last].events = 0;

                /* Shrink logical end, continue loop from here. */
                last--;
                dc->u.h3.npoll--;

            } else {
                /* no hole here, advance to next */
                i++;
            }
        }

        dc->u.h3.first_dead = 0;
        PollsetPrint(dc, "after consolidate", NS_TRUE);
    }
}


/*======================================================================
 * Function Implementations:  NaviServer Interface
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * NsTimeToTimeval --
 *
 *      Convert an Ns_Time structure (NaviServer’s internal time type)
 *      into a POSIX-compatible struct timeval for use with system calls
 *      such as select(), poll(), or gettimeofday()-style APIs.
 *
 *      Handles platform-specific differences between Windows and POSIX:
 *        - On Windows, timeval fields are of type long.
 *        - On POSIX systems, tv_usec is typically suseconds_t.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Writes the converted seconds and microseconds into *dst.
 *      On 32-bit Windows, seconds may truncate beyond the year 2038.
 *
 *----------------------------------------------------------------------
 */
static void NsTimeToTimeval(const Ns_Time *src, struct timeval *dst)
{
    time_t sec  = src->sec;
    long   usec = src->usec;

#if defined(_WIN32)
    /* Windows: struct timeval.tv_usec is long */
    dst->tv_sec  = (long)sec;   /* beware: truncates beyond 2038 */
    dst->tv_usec = (long)usec;
#else
    /* POSIX: struct timeval.tv_usec is suseconds_t */
    dst->tv_sec  = (time_t)sec;
    dst->tv_usec = (suseconds_t)usec;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * SockEnsureReqHeaders --
 *
 *      Ensure that the given HTTP/3 stream has an initialized Ns_Set
 *      for its request headers. This helper wraps NsSockEnsureRequest()
 *      to guarantee that the associated Sock and Request structures
 *      exist and are properly set up before returning the header set.
 *
 * Results:
 *      Returns a pointer to the Ns_Set containing the request headers.
 *
 * Side effects:
 *      May allocate and initialize a Request structure if it does not
 *      already exist for the given StreamCtx’s associated Ns_Sock.
 *
 *----------------------------------------------------------------------
 */
static inline Ns_Set *
SockEnsureReqHeaders(StreamCtx *sc)
{
    return NsSockEnsureRequest((Sock *)sc->nsSock)->headers;
}

/*
 *----------------------------------------------------------------------
 *
 * SockDispatchFinishedRequest --
 *
 *      Dispatch a fully received HTTP/3 request for processing once its
 *      body (if any) has been completely received. This function ensures
 *      that each StreamCtx is dispatched at most once by claiming its
 *      dispatch flag via StreamCtxClaimDispatch().
 *
 *      Depending on the configured upload mode, the request body is either:
 *        - Backed by a temporary file (sockPtr->tfd != NS_INVALID_FD), or
 *        - Stored in memory using the Tcl_DString buffer in reqPtr->buffer.
 *
 *      In the in-memory case, this function finalizes the buffer with a
 *      trailing NUL, updates reqPtr->content and reqPtr->next to point to
 *      the in-memory body, and then calls NsDispatchRequest() to execute
 *      the standard OpenACS/NaviServer request dispatch logic.
 *
 * Results:
 *      NS_OK on successful dispatch; otherwise, the result of
 *      NsDispatchRequest().
 *
 * Side effects:
 *      - Marks the stream as dispatched via StreamCtxClaimDispatch().
 *      - May finalize or prepare request content (file-backed or in-memory).
 *      - Invokes NsDispatchRequest(), which may trigger application-level
 *        request handling and response generation.
 *      - Emits detailed diagnostic logs for tracing request dispatch.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
SockDispatchFinishedRequest(StreamCtx *sc)
{
    Ns_ReturnCode result = NS_OK;
    Sock          *sockPtr = (Sock *)sc->nsSock;

    Ns_Log(Notice, "[%lld] H3[%lld] SockDispatchFinishedRequest %.2x",
           (long long)sc->cc->dc->iter, (long long)sc->quic_sid, sc->io_state);

    /* Avoid double dispatch for the same stream */
    if (StreamCtxClaimDispatch(sc)) {
        Request *reqPtr = sockPtr->reqPtr;

        if (sockPtr->tfd != NS_INVALID_FD) {
            assert(reqPtr->content == NULL);
            Ns_Log(Notice, "[%lld] H3[%lld] SockDispatchFinishedRequest tfd %d (content-length %ld)",
                   (long long)sc->cc->dc->iter, (long long)sc->quic_sid, sockPtr->tfd, reqPtr->contentLength);
        } else {
            Tcl_DStringAppend(&reqPtr->buffer, "", 1);   /* trailing NUL */
            reqPtr->content = reqPtr->buffer.string;     /* body only */

            Ns_Log(Notice, "[%lld] H3[%lld] SockDispatchFinishedRequest buffer %p length %d (content-length %ld)",
                   (long long)sc->cc->dc->iter, (long long)sc->quic_sid, (void*) reqPtr->content, reqPtr->buffer.length, reqPtr->contentLength);
            //Ns_Log(Notice, "[%lld] H3[%lld] SockDispatchFinishedRequest body\n%s",
            //       (long long)sc->cc->dc->iter, (long long)sc->quic_sid, reqPtr->content);

            reqPtr->next    = reqPtr->content;
        }

        result = NsDispatchRequest(sockPtr);
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Module initialization callback for the "quic" (QUIC) driver.
 *      Registers the UDP/QUIC transport in NaviServer, sets up its
 *      driver callbacks, and creates an OpenSSL TLS context using
 *      the QUIC-specific method (OSSL_QUIC_server_method).
 *
 * Parameters:
 *      server   - Name/path of the current server (may be NULL when
 *                 loaded globally).
 *      module   - The module name
 *
 * Returns:
 *      NS_OK    if the driver and its SSL context were initialized
 *               successfully.
 *      NS_ERROR on any failure (configuration lookup, driver registration,
 *               TLS context creation, or ex_data index allocation).
 *
 * Side Effects:
 *      Allocates an NsTLSConfig, registers the driver with Ns_DriverInit(),
 *      creates an SSL_CTX (via Ns_TLS_CtxServerInit), and allocates an SSL
 *      ex_data index for per-connection storage.
 *
 *----------------------------------------------------------------------
 */
NS_EXPORT Ns_ReturnCode Ns_ModuleInit(const char *server, const char *module)
{
    Ns_ReturnCode     result = NS_OK;
    const char       *section, *httpsSection;
    NsTLSConfig      *dc;
    Ns_DriverInitData init;
    Ns_Time           timeout;

    memset(&init, 0, sizeof(init));

    section = Ns_ConfigGetPath(server, module, (char *)0);
    httpsSection = Ns_ConfigString(section, "https", "ns/module/https");

    if (Ns_ConfigGetSection2(httpsSection, NS_FALSE) == NULL) {
        Ns_Log(Error, "quic: linkage to httpsSection <%s> failed", httpsSection);
        return NS_ERROR;
    }

    //httpsConfigSet = Ns_ConfigGetSection(httpsSection);

    /*
     * Load parameters from the specified section
     */
    dc = NsTLSConfigNew(httpsSection);
    Ns_Log(Notice, "Ns_ModuleInit <%s> <%s> has dc %p", server, module, (void*)dc);

    dc->u.h3.npoll        = (size_t)-1;   /* so first PollsetAdd lands at index 0 */
    dc->u.h3.nr_listeners = 0;

    PollsetInit(dc);

    dc->u.h3.recvbufsize = (size_t)Ns_ConfigMemUnitRange(section, "recvbufsize", "8MB",
                                                         1024*8000, 0, INT_MAX);
    Ns_ConfigTimeUnitRange(section, "idletimeout",
                           "3s", 0, 0, LONG_MAX, 0,
                           &timeout);
    NsTimeToTimeval(&timeout, &dc->u.h3.idle_timeout);
    Ns_ConfigTimeUnitRange(section, "draintimeout",
                           "10ms", 0, 0, LONG_MAX, 0,
                           &timeout);
    NsTimeToTimeval(&timeout, &dc->u.h3.drain_timeout);

    Ns_MutexInit(&dc->u.h3.waker_lock);

    init.version = NS_DRIVER_VERSION_6;
    init.name = "quic";
    init.listenProc = Listen;
    init.acceptProc = Accept;
    init.recvProc = Recv;
    init.requestProc = NULL;
    init.sendProc = Send;
    init.sendFileProc = NULL;
    init.keepProc = Keep;
    init.connInfoProc = ConnInfo;
    init.closeProc = Close;
    //init.opts = NS_DRIVER_ASYNC | NS_DRIVER_UDP | NS_DRIVER_QUIC;
    init.opts = NS_DRIVER_UDP | NS_DRIVER_QUIC;
    init.arg = dc;
    init.path = httpsSection;  // used for getting address and port etc.
    init.protocol = "https";
    init.defaultPort = 443;
    init.driverThreadProc  = QuicThread;
    init.headersEncodeProc = h3_stream_build_resp_headers;

    // TODO: should we handle vhostcertificates?


    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "quic: driver init failed.");
        ns_free(dc);
        result = NS_ERROR;

    } else {
        /*
         * Create an SSL_CTX the same way as for HTTP/1.*, but by passing NS_DRIVER_UDP,
         * we are using
         *
         *     server_method = OSSL_QUIC_server_method();
         *     ctx = SSL_CTX_new(server_method);
         *
         * instead of TLS_server_method().
         */
        static char conn_ctx_tag[] = "ConnCtx";
        static char stream_ctx_tag[] = "StreamCtx";
        int rc = Ns_TLS_CtxServerInit(httpsSection, NULL, NS_DRIVER_QUIC|NS_DRIVER_SNI,
                                      dc, &dc->ctx);
        Ns_Log(Notice, "quic: created sslCtx %p for dc %p",
               (void*)dc->ctx, (void*)dc);

        if (rc != TCL_OK) {
            Ns_Log(Error, "nsssl: could not initialize OpenSSL context for QUIC (section %s): %s",
                   section, strerror(errno));
            result = NS_ERROR;
        } else {
            uint64_t domain_flags = (uint64_t)-1;

            SSL_CTX_get_domain_flags(dc->ctx, &domain_flags);
            Ns_Log(Notice, "quic: created sslCtx %p, num tickets %ld domain_flags %02" PRIx64,
                   (void*)dc->ctx, SSL_CTX_get_num_tickets(dc->ctx), domain_flags);

            if (SSL_CTX_set_domain_flags(dc->ctx, SSL_DOMAIN_FLAG_THREAD_ASSISTED) != 1) {
                Ns_Log(Error, "QUIC: SSL_CTX_set_domain_flags(THREAD_ASSISTED) failed");
            }

            /*
             * Keep as default a no explicit settings config. This means NST +
             * resumption ON, 0-RTT OFF.
             *
             * Working config = NSTs + resumption, no 0-RTT.
             */
            //SSL_CTX_set_num_tickets(dc->ctx, 2);              // default in OpenSSL
            //SSL_CTX_set_max_early_data(dc->ctx, 0);           // don’t advertise 0-RTT

            /*
             * The following deactivates stateless session tickets.
             */
            //SSL_CTX_set_options(dc->ctx, SSL_OP_NO_TICKET);

            // FF can balk at hybrid KEM groups in QUIC
            //SSL_CTX_set1_groups_list(dc->ctx, "X25519:P-256");

        }
        dc->u.h3.cc_idx = SSL_get_ex_new_index(0, conn_ctx_tag, NULL, NULL, ossl_cc_exdata_free);
        dc->u.h3.sc_idx = SSL_get_ex_new_index(0, stream_ctx_tag, NULL, NULL, ossl_sc_exdata_free);

        Ns_Log(Notice, "H3 set ex_data indices cc_idx %d sc_idx %d", dc->u.h3.cc_idx, dc->u.h3.sc_idx);

        if (result == TCL_OK && (dc->u.h3.cc_idx < 0 || dc->u.h3.sc_idx < 0)) {
            Ns_Log(Error, "quic: Could not allocate SSL ex_data index");
            result = NS_ERROR;
        }
    }

    if (result != NS_ERROR) {
        Ns_Log(Notice, "quic: driver loaded");
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * QuicThread --
 *
 *      Main event-loop thread for the HTTP/3/QUIC driver using the
 *      OpenSSL QUIC APIs. Created via Ns_ThreadCreate() in driver.c,
 *      it initializes nghttp3 callbacks/allocators, signals driver
 *      readiness, and then repeatedly:
 *
 *        - polls all listener, connection, and stream SSL* objects via
 *          SSL_poll();
 *        - drives QUIC timers/state with SSL_handle_events();
 *        - accepts new QUIC connections/streams when signaled (IC/ISB/ISU);
 *        - advances TLS handshakes (OSB/OSU) and configures server uni streams;
 *        - services readable streams (R) and schedules writers (W);
 *        - handles error/edge conditions (EC/ECD/ER/EW/EL) and performs
 *          orderly teardown;
 *        - performs post-loop maintenance (PollsetSweep/Consolidate) and
 *          updates per-connection poll interest.
 *
 *      The loop adapts its poll timeout between an idle timeout and a
 *      shorter "drain" timeout depending on whether any connection has
 *      pending write work or producer resumption to flush.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Modifies driver state/flags and broadcasts readiness/stop to listeners.
 *      - Accepts connections/streams; creates and frees ConnCtx/StreamCtx.
 *      - Reads from and writes to UDP sockets through OpenSSL’s QUIC engine.
 *      - Updates poll masks, moves items within internal poll arrays, and
 *        frees SSL objects during sweep.
 *      - Emits diagnostic logs for state transitions, errors, and I/O.
 *
 *----------------------------------------------------------------------
 */
static void
QuicThread(void *arg)
{
    Driver             *drvPtr = (Driver*)arg;
    TCL_SIZE_T          nrBindaddrs;
    NsTLSConfig        *dc = drvPtr->arg;
    SSL_CTX            *h3ctx;
    bool                stopping = NS_FALSE;
    unsigned int        flags = NS_DRIVER_THREAD_STARTED;
    struct timeval     *polltimeout_ptr;

    Ns_ThreadSetName("-quic:%s-", "h3");
    Ns_Log(Notice, "H3D QUIC THREAD started");

    nrBindaddrs = NsDriverBindAddresses(drvPtr);

    if (nrBindaddrs > 0) {
        NsDriverStartSpoolers(drvPtr);
        flags |= NS_DRIVER_THREAD_READY;
    } else {
        flags |= (NS_DRIVER_THREAD_FAILED | NS_DRIVER_THREAD_SHUTDOWN);
    }
    fprintf(stderr, "DEBUG: QUIC lock driver %p\n", (void*)drvPtr->lock);
    Ns_MutexLock(&drvPtr->lock);
    drvPtr->flags |= flags;
    fprintf(stderr, "DEBUG: QUIC BROADCAST flags %.2x\n", flags);
    Ns_CondBroadcast(&drvPtr->cond);
    Ns_MutexUnlock(&drvPtr->lock);

    h3ctx = dc->ctx;          // defined by Listen()

    (void) h3ctx;

    /*
     * At this time, the following things happened already:
     *  - SSL listener created
     *  - listener connected for the UDP socket
     *  - listener is listening via SSL_listen
     *  - listener is nonblocking
     */
    h3_callbacks.recv_settings        = on_recv_settings;
    h3_callbacks.begin_headers        = on_begin_headers;
    h3_callbacks.recv_header          = on_recv_header;
    h3_callbacks.end_headers          = on_end_headers;
    h3_callbacks.recv_data            = on_recv_data;
    h3_callbacks.end_stream           = on_end_stream;
    h3_callbacks.acked_stream_data    = on_acked_stream_data;
    h3_callbacks.stream_close         = on_stream_close;
    h3_callbacks.deferred_consume     = on_deferred_consume;

    h3_mem.user_data = NULL;
    h3_mem.malloc    = h3_malloc_cb;
    h3_mem.free      = h3_free_cb;
    h3_mem.calloc    = h3_calloc_cb;
    h3_mem.realloc   = h3_realloc_cb;

    polltimeout_ptr = &dc->u.h3.idle_timeout;

    while (!stopping) {
        int      i, ret;
        size_t   result_count = SIZE_MAX, numitems;
        uint64_t processed_event = 0;

        /*
         * Process all the streams
         * the first one is the connection if we get something here is a new stream
         */

        numitems = PollsetCount(dc);

        Ns_Log(Notice, "[%lld] H3D calling SSL_poll with %ld items timeout " NS_TIME_FMT,
               (long long)dc->iter, numitems,
               (int64_t)polltimeout_ptr->tv_sec, (long)polltimeout_ptr->tv_usec
               );

        dc->iter++;
        //if (dc->iter>10000) { char *p=NULL; *p=0; }

        ret = SSL_poll(dc->u.h3.poll_items, numitems, sizeof(SSL_POLL_ITEM), polltimeout_ptr,
                       SSL_POLL_FLAG_NO_HANDLE_EVENTS, &result_count);

        Ns_Log(Notice, "[%lld] H3D SSL_poll returns rc %d with %ld items with events"
               " (quic.c from %s %s)",
               (long long)dc->iter, ret, result_count, __DATE__, __TIME__);

        for (i = 0; i < (int)numitems; i++) {
            SSL           *s       = dc->u.h3.ssl_items.data[i];
            SSL_POLL_ITEM *item    = &dc->u.h3.poll_items[i];
            uint64_t       revents = item->revents;
            Tcl_DString    ds1, ds2;
            ConnCtx       *cc = NULL;
            StreamCtx     *sc = NULL;

            if (s != NULL) {
                sc = SSL_get_ex_data(s, dc->u.h3.sc_idx);
                if (sc != NULL) {
                    cc = sc->cc;
                } else {
                    cc = SSL_get_ex_data(s, dc->u.h3.cc_idx);
                }
            }

            Tcl_DStringInit(&ds1);
            Tcl_DStringInit(&ds2);
            Ns_Log(Notice, "[%lld] H3D poll item %d: s %p (%s)"
                   " events %04" PRIx64 " %s"
                   " revents %04" PRIx64 " %s",
                   (long long)dc->iter, i,
                   (void*)s,
                   cc == NULL ? "listener" :
                   s == cc->h3ssl.conn ? "conn" :
                   (s != NULL && sc != NULL) ? H3StreamKind_str(sc->kind) : "hole",
                   item->events, DStringAppendSslPollEventFlags(&ds1, item->events),
                   revents, DStringAppendSslPollEventFlags(&ds2, revents)
                   );
            Tcl_DStringFree(&ds1);
            Tcl_DStringFree(&ds2);
        }

        if (ret == 0) {
            Ns_Log(Error, "[%lld] H3D SSL_poll failed", (long long)dc->iter);
            continue;
        }
        if (result_count == 0) {
            /* Timeout may be something somewhere */
            Ns_Log(Notice, "[%lld] H3D timeout", (long long)dc->iter);
            (void)PollsetHandleListenerEvents(dc);
            //continue;
        }

        /* reset the states */
        //h3ssl->done     = NS_FALSE;
        dc->u.h3.first_dead   = 0;

        /*
         * Process all the items we have polled. We have to be careful, in
         * cases, when items are deleted, since deletion causes a swap with
         * the last element. So we need some mark-for-delete (e.g. just
         * setting conns[i] to NULL) and perform the deletion (swap) in a
         * second step.
         */
        for (i = 0; i < (int)numitems; i++) {
            SSL           *s        = dc->u.h3.ssl_items.data[i];
            SSL_POLL_ITEM *item     = &dc->u.h3.poll_items[i];
            uint64_t       revents  = item->revents;
            ConnCtx       *cc;
            StreamCtx     *sc;
            Tcl_DString    ds;

            //Ns_Log(Notice, "[%lld] H3D %p item %d: stream %p events %.8llx revents %.8llx",
            //       (long long)dc->iter, (void*)item, i, (void*)s, item->events, revents);

            if (s == NULL) {
                continue;                       /* hole created by PollsetMarkDead */
            }
            if (revents == SSL_POLL_EVENT_NONE) {
                continue;
            }

            /*
             * First try to get sc from the stream. If we succeed, get cc from
             * sc; otherwise, try to get cc from the stream.
             */
            sc = SSL_get_ex_data(s, dc->u.h3.sc_idx);
            if (sc != NULL) {
                cc = sc->cc;
            } else {
                cc = SSL_get_ex_data(s, dc->u.h3.cc_idx);
            }

            if (cc == NULL && i > (int)dc->u.h3.nr_listeners - 1 ) {
                Ns_Log(Notice, "[%lld] H3D item %d: cannot get cc for stream %p",
                       (long long)dc->iter, i, (void*)s);
            }
            if (i > (int)dc->u.h3.nr_listeners - 1) {
                assert(cc != NULL);
            }
            if (cc != NULL) {
                /*
                 * Log just for streams, not for listeners
                 */
                ossl_conn_maybe_log_first_shutdown(cc, "event processing");
            }

            /*Tcl_DStringInit(&ds);
              Ns_Log(Notice, "[%lld] H3D item %d: revents %.8llx %s from stream %p",
              (long long)dc->iter, i, revents, DStringAppendSslPollEventFlags(&ds, revents), (void*)s);
              Tcl_DStringFree(&ds);*/

            processed_event = 0;

            {
                Tcl_DStringInit(&ds);
                Ns_Log(Notice, "[%lld] H3D processing poll item %d: s %p (%s)"
                       " revents %08" PRIx64 " %s",
                       (long long)dc->iter, i,
                       (void*)s,
                       cc == NULL ? "listener" :
                       s == cc->h3ssl.conn ? "conn" :
                       sc != NULL ? H3StreamKind_str(sc->kind) : "???",
                       revents, DStringAppendSslPollEventFlags(&ds, revents)
                       );
                Tcl_DStringFree(&ds);
            }

            if (revents & (SSL_POLL_EVENT_ISB
                           | SSL_POLL_EVENT_ISU
                           | SSL_POLL_EVENT_EC
                           | SSL_POLL_EVENT_ECD)) {
                int spins = 0;
                for (;;) {

                    Ns_Log(Notice, "[%lld] H3D poll item %d: preprocessing event loop, iteration %d", (long long)dc->iter, i, spins);

                    (void)SSL_handle_events(cc->h3ssl.conn);
                    Ns_Log(Notice, "[%lld] H3D poll item %d: preprocessing event loop, itertion %d DONE", (long long)dc->iter, i, spins);
                    spins++;

                    /* Stop when OpenSSL wants a future wakeup (non-zero timeout), or after a few spins */
                    //struct timeval tv; int is_inf = 0;
                    //if (SSL_get_event_timeout(cc->h3ssl.conn, &tv, &is_inf) != 1) break;
                    //if (!is_inf && (tv.tv_sec > 0 || tv.tv_usec > 0)) break;
                    if (spins >= 3) break;   /* safety cap to avoid tight spin */
                }
                //did_progress = NS_TRUE;
            }


            if (revents & SSL_POLL_EVENT_IC) {
                // incoming connection
                Ns_Log(Notice, "[%lld] H3D item %d: received POLL_EVENT_IC provided cc %p", (long long)dc->iter, i, (void*)cc);

                quic_conn_handle_ic(s, drvPtr);

                cc = SSL_get_ex_data(s, dc->u.h3.cc_idx);
                Ns_Log(Notice, "[%lld] H3D item %d: received POLL_EVENT_IC processed", (long long)dc->iter, i);
                processed_event |= SSL_POLL_EVENT_IC;
            }

            if ((revents & (SSL_POLL_EVENT_OSB|SSL_POLL_EVENT_OSU)) != 0) {
                if (cc->handshake_done) {
                    Ns_Log(Notice, "[%lld] H3D item %d: processing OSB|OSU handshake done %d",
                           (long long)dc->iter, i, cc->handshake_done);
                }

                if (revents & SSL_POLL_EVENT_OSB) {
                    processed_event |= SSL_POLL_EVENT_OSB;
                }
                if (revents & SSL_POLL_EVENT_OSU) {
                    processed_event |= SSL_POLL_EVENT_OSU;
                }

                if (!cc->handshake_done) {
                    int hs_result = quic_conn_drive_handshake(dc, s);
                    //ossl_log_handshake_state(s);

                    Ns_Log(Notice, "[%lld] H3D item %d: processing OSB|OSU drive_hand_shake -> %d",
                           (long long)dc->iter, i, hs_result);

                    if (hs_result == 1) {
                        int rc;

                        cc->handshake_done = NS_TRUE;

                        OSSL_TRY((rc = SSL_set_incoming_stream_policy(cc->h3ssl.conn,
                                                                      SSL_INCOMING_STREAM_POLICY_ACCEPT,
                                                                      0)));
                        ossl_conn_maybe_log_first_shutdown(cc, "OSB|OSU after incoming stream policy set");

                        if (rc != 1) {
                            ossl_log_error_detail(rc, "set_incoming_stream_policy(conn)");
                        }

                        Ns_Log(Notice, "[%lld] H3D item %d: processing OSB|OSU creates server streams",
                               (long long)dc->iter, i);
                        if (quic_conn_open_server_uni_streams(cc, &cc->h3ssl) == 0) {
                            ossl_conn_maybe_log_first_shutdown(cc, "OSB|OSU after quic_conn_open_server_uni_streams");
                            //hassomething++;
                        } else {
                            Ns_Log(Error, "H3: failed to create server uni streams; leaving conn up for now");
                        }
                    } else if (hs_result == -1) {
                        // Hard stream error - remove connection
                        PollsetMarkDead(cc, s, "OSB|OSU handshake failed");
                        goto skip;
                    }
                }
            }

            if (revents & (SSL_POLL_EVENT_ISB | SSL_POLL_EVENT_ISU)) {
                /*
                 * Incoming bidi or uni stream
                 */
                if (revents & SSL_POLL_EVENT_ISB) {
                    processed_event |= SSL_POLL_EVENT_ISB;
                }
                if (revents & SSL_POLL_EVENT_ISU) {
                    processed_event |= SSL_POLL_EVENT_ISU;
                }

                if (!cc->handshake_done) {
                    Ns_Log(Notice, "[%lld] H3D[%d] Deferring ISB|ISU until handshake completes",
                           (long long)dc->iter, i);
                } else {
                    unsigned accepted = 0;
                    const unsigned max_accept = 64; /* starvation guard */

                    Ns_Log(Notice, "[%lld] H3D item %d: processing ISB|ISU,"
                           " attempting to accept %ld streams %s",
                           (long long)dc->iter, i, SSL_get_accept_stream_queue_len(cc->h3ssl.conn),
                           (revents & SSL_POLL_EVENT_EC) != 0 ? " with EC" : "");

                    for (;;) {
                        SSL *stream;

                        if (accepted >= max_accept) {
                            Ns_Log(Notice, "[%lld] H3D item %d: accepted %u streams (cap), will continue next tick",
                                   (long long)dc->iter, i, accepted);
                            break;
                        }
                        stream = SSL_accept_stream(cc->h3ssl.conn, 0);
                        if (stream == NULL) {
                            quic_stream_accepted_null(cc);
                            break;  /* always exit loop on stream == NULL */

                        } else {
                            int st = SSL_get_stream_type(stream);
                            ++accepted;
                            if (st == SSL_STREAM_TYPE_READ) {
                                /* client-initiated unidirectional */
                                PollsetAddStreamRegister(cc, stream, H3_KIND_CLIENT_UNI);

                            } else if (st == SSL_STREAM_TYPE_BIDI) {
                                /* client-initiated request stream */
                                sc = PollsetAddStreamRegister(cc, stream, H3_KIND_BIDI_REQ);
                                Ns_Log(Notice, "[%lld] H3D item %d: registered BIDI with cc %p sc %p nsSock %p",
                                       (long long)dc->iter, i, (void*)cc, (void*)sc, (void*)sc->nsSock);
                                cc->h3ssl.bidi_sid = sc->quic_sid;

                            } else {
                                Ns_Log(Warning, "[%lld] H3D item %d: unexpected incoming stream"
                                       " with type %s", (long long)dc->iter, i, ossl_quic_stream_type_str(st));
                                SSL_shutdown(stream);
                            }
                        }
                        ossl_conn_maybe_log_first_shutdown(cc, "accept_and_register_new_stream DONE");
                    }
                }
            }

            if (revents & SSL_POLL_EVENT_R) {
                //Ns_Log(Notice, "[%lld] H3D[%lld] item %d: processing R", (long long)dc->iter, (long long)sid, i);
                processed_event |= SSL_POLL_EVENT_R;

                if (quic_stream_handle_r(cc, s)) {
                    /* stream became dead; skip further work on this slot */
                    goto skip;
                }
            }

            if ((revents & (SSL_POLL_EVENT_W)) != 0) {
                Ns_Log(Notice, "[%lld] H3[%lld] processing W", (long long)dc->iter, (long long)sc->quic_sid);
                if (StreamCtxIsServerUni(sc)) {
                    /*
                     * Idle control/QPACK streams often look writable forever.
                     * Disarm W to avoid busy looping; the writer will still run
                     * when cc->wants_write is set.
                     */
                    PollsetDisableWrite(dc, s, sc, "Event W, Idle control/QPACK stream");
                } else {
                    SharedSnapshot snap = SharedSnapshotInit(&sc->sh);
                    if (SharedEOFReady(&snap)) {
                        h3_stream_maybe_finalize(sc, "event W");
                    } else {
                        cc->wants_write = NS_TRUE;
                    }
                }
                processed_event |= SSL_POLL_EVENT_W;
            }

            if ((revents & (SSL_POLL_EVENT_EC
                            |SSL_POLL_EVENT_ER
                            |SSL_POLL_EVENT_EW))
                && s == cc->h3ssl.conn) {
                processed_event |= (revents & ((SSL_POLL_EVENT_EC
                                                |SSL_POLL_EVENT_ER
                                                |SSL_POLL_EVENT_EW)));
                if (quic_conn_handle_e(cc, s, revents)) {
                    dc->u.h3.poll_items[i].revents = 0;  /* avoid reprocessing this (now-dead) slot */
                    goto skip;
                }
            }

            if ((revents & (SSL_POLL_EVENT_ER|SSL_POLL_EVENT_EW))
                && (s != cc->h3ssl.conn)) {
                int64_t sid = (int64_t)SSL_get_stream_id(s);

                processed_event |= (revents & ((SSL_POLL_EVENT_ER|SSL_POLL_EVENT_EW)));
                if (sid >= 0) {
                    quic_stream_handle_e(cc, s, (uint64_t)sid, item->revents, item->events);
                    goto skip;
                }
            }

            if (revents & SSL_POLL_EVENT_EL) {
                processed_event |= (revents & ((SSL_POLL_EVENT_EL)));

                Ns_Log(Notice, "[%lld] H3D item %d: Received EL, but not yet processed", (long long)dc->iter, i);
            }


            if (revents != processed_event) {
                uint64_t sid = UINT64_MAX, not_processed = (revents & ~processed_event);
                Tcl_DString ds1;

                Tcl_DStringInit(&ds1);
                sid = SSL_get_stream_id(item->desc.value.ssl);
                Tcl_DStringInit(&ds);
                Ns_Log(Notice, "[%lld] H3D item %d: s %p sid %lld"
                       " item->re %08" PRIx64
                       " revents %08" PRIx64 " %s != %08" PRIx64 " -> NOT PROCESSED %s",
                       (long long)dc->iter, i, (void*)dc->u.h3.ssl_items.data[i],
                       (long long)sid, item->revents, revents,
                       DStringAppendSslPollEventFlags(&ds, revents), processed_event,
                       DStringAppendSslPollEventFlags(&ds1, not_processed)
                       );
                Tcl_DStringFree(&ds);
                Tcl_DStringFree(&ds1);
            }

        skip:
            /*
             * clear event mask
             */
            item->revents = SSL_POLL_EVENT_NONE;
            //Ns_Log(Notice, "[%lld] get next event", (long long)dc->iter);
        }

        /*
         * All events are processed
         *
         * Write to all connections that reported write demands.
         */
        //Ns_Log(Notice, "[%lld] all events processed", (long long)dc->iter);

        {
            bool expecting_send = NS_FALSE;

            if (dc->u.h3.conns.size > 0) {
                for (i = 0u; i < (int)dc->u.h3.conns.size; i++) {
                    ConnCtx *cc = dc->u.h3.conns.data[i];

                    Ns_Log(Notice, "[%lld] all events processed conn[%d] cc->expecting_send %d cc->wants_write %d"
                           " has resume pending %d",
                           (long long)dc->iter, i, cc->expecting_send, cc->wants_write,
                           SharedHasResumePending(&cc->shared));

                    if (cc->expecting_send) {
                        Ns_Log(Notice, "[%lld] H3D cc %p expecting send", (long long)dc->iter, (void*)cc);
                        expecting_send = NS_TRUE;
                        cc->expecting_send = NS_FALSE;
                    }
                    if (cc->wants_write) {
                        //Ns_Log(Notice, "[%lld] H3D write demand from cc %p", (long long)dc->iter, (void*)cc);
                        cc->wants_write = h3_conn_write_step(cc);
                        Ns_Log(Notice, "[%lld] H3D after h3_conn_write_step cc %p", (long long)dc->iter, (void*)cc);
                    }

                    /* Decide whether we should run another drain pass without sleeping. */
                    if (cc->wants_write) {
                        Ns_Log(Notice, "[%lld] H3D cc %p cc->wants_write is still set", (long long)dc->iter, (void*)cc);
                        expecting_send = NS_TRUE;
                    }
                    //Ns_Log(Notice, "[%lld] H3D conn loop SSL_handle_events conn %p => %d",
                    //       (long long)dc->iter, (void*)cc->h3ssl.conn, SSL_handle_events(cc->h3ssl.conn));

                    PollsetUpdateConnPollInterest(cc);
                }
                polltimeout_ptr = expecting_send ? &dc->u.h3.drain_timeout : &dc->u.h3.idle_timeout;
            }
            PollsetSweep(dc);
            PollsetConsolidate(dc);
            //Ns_Log(Notice, "[%lld] H3D after consolidate, npoll %ld", (long long)dc->iter, PollsetCount(dc));
        }
    }

    // Cleanup missing
    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&drvPtr->lock);
    drvPtr->flags |= NS_DRIVER_THREAD_STOPPED;
    Ns_CondBroadcast(&drvPtr->cond);
    Ns_MutexUnlock(&drvPtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * Listen --
 *
 *      Called by the NaviServer driver framework when the QUIC listener
 *      socket is brought up. This function opens a UDP socket bound to
 *      the specified address and port, attaches it to a nonblocking
 *      OpenSSL QUIC listener (`SSL_new_listener()`), and registers the
 *      listener as a permanent entry in the HTTP/3 pollset.
 *
 *      The listener SSL object is responsible for accepting incoming
 *      QUIC connections via SSL_poll() events (IC/F/EL/EC/ECD). Its
 *      file descriptor is integrated into the event loop and stored in
 *      the driver’s NsTLSConfig (`dc`) structure.
 *
 * Results:
 *      Returns the opened UDP socket descriptor on success, or
 *      NS_INVALID_SOCKET on failure.
 *
 * Side effects:
 *      - Binds and configures a UDP socket for QUIC traffic.
 *      - Creates and initializes a QUIC listener (`SSL *listener`).
 *      - Sets nonblocking mode and receive buffer size.
 *      - Records listener address in `dc->u.h3.waker_addr` for wakeups.
 *      - Inserts listener into the pollset and increments
 *        `dc->u.h3.nr_listeners`.
 *      - Logs detailed initialization and error diagnostics.
 *
 *----------------------------------------------------------------------
 */
static NS_SOCKET
Listen(Ns_Driver *driver, const char *address, unsigned short port, int UNUSED(backlog), bool reuseport)
{
    NS_SOCKET    sock;
    SSL         *listener = NULL;
    NsTLSConfig *dc = driver->arg;

    assert(dc);

    sock = Ns_SockListenUdp(address, port, reuseport);
    Ns_Log(Notice, "[%lld] H3 listen <%s> port %hu -> sock %d", (long long)dc->iter, address, port, sock);
    if (sock != NS_INVALID_SOCKET) {
        size_t idx;

        Ns_Log(Notice, "[%lld] H3 listen has ctx %p", (long long)dc->iter, (void*)dc->ctx);
        if (dc->ctx != NULL) {
            dc->driver = driver;
            Ns_Log(Notice, "[%lld] H3 listen set driver %p in dc %p", (long long)dc->iter, (void*)driver, (void*)dc);

            listener = SSL_new_listener(dc->ctx, 0);
            if (listener == NULL) {
                goto fail;
            }

            OSSL_TRY(SSL_set_fd(listener, sock));
            OSSL_TRY(SSL_set_blocking_mode(listener, 0));
            if (!SSL_listen(listener)) {
                /* log error */
                goto fail;
            } else {
                struct sockaddr_storage sa;
                socklen_t slen = (socklen_t)sizeof(sa);

                if (getsockname(sock, (struct sockaddr *)&sa, &slen) == 0) {
                    if (Ns_SockaddrInAny((struct sockaddr *)&sa)) {
                        /*
                         * If bound to wildcard, replace with loopback of the same family
                         */
                        Ns_SockaddrSetLoopback((struct sockaddr *)&sa);
                    }

                    /* Store a copy in out driver/config for later sendto() */
                    memcpy(&dc->u.h3.waker_addr, &sa, slen);
                    dc->u.h3.waker_addrlen    = slen;
                } else {
                    Ns_Log(Error, "H3 listen: getsockname() failed on fd %d", (int)sock);
                }
            }

        } else {
            Ns_Log(Error, "H3 context not initialized for <%s> %hu sock %d",
                   address, port, sock);
            goto fail;
        }
        (void) Ns_SockSetNonBlocking(sock);
        quic_udp_set_rcvbuf(sock, dc->u.h3.recvbufsize);

        /*
         * Set app data of listener ssl to listen fd
         */

        /* Add this listener as a permanent prefix entry in the pollset */
        idx = PollsetAdd(dc, listener,
                         SSL_POLL_EVENT_IC | SSL_POLL_EVENT_F | SSL_POLL_EVENT_EL |
                         SSL_POLL_EVENT_EC | SSL_POLL_EVENT_ECD,
                         /* maskf */   NULL,
                         /* label */   "listener",
                         /* kind  */   H3_KIND_UNKNOWN);

        dc->u.h3.nr_listeners++;         /* remember how many listeners we pinned at the front */
        Ns_Log(Notice, "[%lld] PollsetAdd for listener returned %ld, nr_listeners %ld npoll %ld",
               (long long)dc->iter, idx, dc->u.h3.nr_listeners, PollsetCount(dc));

        ERR_clear_error();
    }
    return sock;
 fail:
    if (listener != NULL) {
        SSL_free(listener);
    }
    if (sock != NS_INVALID_SOCKET) {
        ns_sockclose(sock);
    }
    ERR_clear_error();
    return NS_INVALID_SOCKET;
}


/*
 *----------------------------------------------------------------------
 *
 * Accept --
 *
 *      Driver-level accept callback for the HTTP/3 (QUIC) listener.
 *      Unlike traditional TCP-based drivers, this function is invoked
 *      programmatically via NsSockAccept() rather than through a kernel
 *      accept() call, because QUIC operates over UDP.
 *
 *      The function associates the accepted socket with a freshly
 *      allocated QuicSockCtx, marks it as HTTP/3-aware (is_h3 = NS_TRUE),
 *      and stores the SSL or connection reference (if available).
 *      For OpenSSL 4+, the peer address is also bound to the QUIC
 *      connection via quic_conn_set_sockaddr().
 *
 * Results:
 *      Returns NS_DRIVER_ACCEPT_DATA to signal successful acceptance
 *      and readiness for further processing.
 *
 * Side effects:
 *      - Allocates a new QuicSockCtx and attaches it to the Ns_Sock.
 *      - Replaces the socket’s arg pointer (previously SSL*) with the
 *        QuicSockCtx.
 *      - Optionally binds the QUIC peer address into the OpenSSL context.
 *      - Logs diagnostic information about the accepted socket.
 *
 *----------------------------------------------------------------------
 */
static NS_DRIVER_ACCEPT_STATUS
Accept(Ns_Sock *sock, NS_SOCKET listensock,
       struct sockaddr *saPtr, socklen_t *socklenPtr)
{
    QuicSockCtx *qctx;
    NsTLSConfig *dc = ((Sock *)sock)->drvPtr->arg;

    Ns_Log(Notice, "[%lld] H3 Accept sock %d arg %p", (long long)dc->iter, listensock, (void*)sock->arg);

    /*
     * Tag this Ns_Sock as H3 so later code (ns_conn version, etc.)
     * can tell it apart from H1
     */
    qctx        = ns_calloc(1, sizeof(*qctx));
    qctx->is_h3 = NS_TRUE;

    if (sock->arg != NULL) {
        qctx->ssl = sock->arg;

#if HAVE_OPENSSL_4
        quic_conn_set_sockaddr(sock->arg, saPtr, socklenPtr);
#else
        (void)saPtr;
        (void)socklenPtr;
#endif
    }

    /*
     * Change the sock->arg from ssl to qctx.
     */
    sock->arg  = qctx;
    sock->sock = listensock;

    return NS_DRIVER_ACCEPT_DATA;
}

/*
 *----------------------------------------------------------------------
 *
 * Recv --
 *
 *      Placeholder receive callback for the HTTP/3 (QUIC) driver.
 *      Invoked by the NaviServer driver framework when incoming data
 *      is expected to be read into the provided I/O vectors.
 *
 *      The function is not yet implemented.
 *
 *      Unlike TCP drivers, QUIC does not deliver application data
 *      directly through recv(); instead, datagrams are processed by
 *      OpenSSL’s internal QUIC engine, which demultiplexes them into
 *      connections and streams. Therefore, this stub currently just
 *      calls PollsetHandleListenerEvents() to advance the QUIC reactor
 *      and will be expanded later to feed stream data into NaviServer’s
 *      request layer.
 *
 * Results:
 *      Returns the total number of bytes received (currently always 0),
 *      or -1 on error or timeout.
 *
 * Side effects:
 *      - Drives OpenSSL’s QUIC event handling for listener sockets.
 *      - Logs diagnostic information about pending QUIC I/O.
 *      - Triggers a controlled crash placeholder to highlight
 *        unimplemented functionality.
 *
 *----------------------------------------------------------------------
 */
static ssize_t
Recv(Ns_Sock *sock, struct iovec *bufs, int nbufs,
     Ns_Time *UNUSED(timeoutPtr), unsigned int UNUSED(flags))
{
    NsTLSConfig *dc = sock->driver->arg;
    ssize_t      produced_total = 0;

    if (nbufs <= 0) {
        /*
         * Nothing to fill, but still drive I/O so QUIC progresses
         */
    }

    // Not implemented yet, let the server crash to see how this was called
    Ns_Log(Notice, "H3 Recv (sock %d) nbufs %d", sock->sock, nbufs);
    Ns_Log(Error, "H3 Recv (sock %d) %p nbufs %d -> NOT IMPLEMENTED YET", sock->sock, (void*)bufs, nbufs);
    (void)raise(SIGSEGV);

    /* Let OpenSSL pull UDP datagrams and dispatch internally */
    (void)PollsetHandleListenerEvents(dc);

    Ns_Log(Notice, "H3 Recv (sock %d) returns %ld bytes", sock->sock, produced_total);
    return produced_total;
}

/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Driver-level send callback for HTTP/3 (QUIC) streams.
 *      Called by the NaviServer driver framework to transmit response
 *      data from one or more I/O vectors. Unlike TCP-based drivers,
 *      this function does not call send() directly - it enqueues
 *      application data into the shared per-stream buffer, to be
 *      consumed asynchronously by the nghttp3 write loop.
 *
 *      The function also ensures that HTTP/3 response headers are staged
 *      exactly once per stream, sets appropriate readiness flags, and
 *      wakes the shared worker thread to drive the write path.
 *
 * Results:
 *      Returns the total number of bytes accepted for transmission,
 *      or -1 on internal error.
 *
 * Side effects:
 *      - May allocate or extend per-stream shared queues.
 *      - Marks headers as ready for submission (SharedHdrsSetReady()).
 *      - Enqueues body data via SharedEnqueueBody().
 *      - Optionally marks the stream as application-closed on NS_SEND_EOF.
 *      - Signals the QUIC pollset to enable write readiness.
 *      - Logs detailed diagnostic output for each send call.
 *
 *----------------------------------------------------------------------
 */
static ssize_t
Send(Ns_Sock *sock, const struct iovec *iov, int niov, unsigned int UNUSED(flags))
{
    NsTLSConfig *dc = sock->driver->arg;
    ssize_t      consumed = 0;
    StreamCtx   *sc;
    int          start_iov = 0, j;
    bool         need_resume = NS_FALSE;

    Ns_Log(Notice, "[%lld] H3 Send (sock %d) nbufs %d", (long long)dc->iter, sock->sock, niov);

    sc = StreamCtxFromSock(dc, sock);
    if (sc == NULL) {
        Ns_Log(Error, "h3: cannot determine H3 stream context from Ns_Sock structure");
        assert(sc != NULL);
    }

    Ns_Log(Notice, "[%lld] H3 Send: cc %p sc %p hdrs_submitted %d hdrs_ready %d nva %p",
           (long long)dc->iter, (void*)sc->cc, (void*)sc, sc->hdrs_submitted, sc->hdrs_ready, (void*)sc->resp_nv);

    if (!H3_TX_WRITABLE(sc)) {          /* honors H3_IO_TX_FIN/H3_IO_RESET */
        return 0;
    }

    /* Stage headers once, using the shared bit */
    if (!sc->hdrs_submitted && !SharedHdrsIsReady(&sc->sh)) {
        SharedHdrsSetReady(&sc->sh);
        need_resume = NS_TRUE;
    }

    /* ----- Enqueue remaining body iovecs (or all if no headers were present) */
    for (j = start_iov; j < niov; ++j) {
        if (iov[j].iov_len > 0) {
            (void)SharedEnqueueBody(&sc->sh, iov[j].iov_base, iov[j].iov_len, "send:body");
            consumed    += (ssize_t)iov[j].iov_len;
            need_resume  = NS_TRUE;
        }
    }

#ifdef PASSED_FLAGS_CAN_CONTAIN_EOF
    /*
     * We could simplify stream end handling when we get information via flags
     * that the current data is the last data chunk.
     */
    if (flags & NS_SEND_EOF) {
        SharedMarkClosedByApp(&sc->sh);
        need_resume  = NS_TRUE;
    }
#endif

    /* ----- One edge-triggered nudge to the consumer (nghttp3 resume) */
    if (need_resume) {
        SharedRequestResume(&sc->cc->shared, &sc->sh, sc->h3_sid);    /* put SID in ring */
        PollsetEnableWrite(dc, sc->ssl, sc, "Send: staged/enqueued");  /* per-stream W */
    }

    Ns_Log(Notice, "[%lld] H3 Send nbufs %d -> DONE (consumed %ld)",
           (long long)dc->iter, niov, consumed);
    return consumed;
}

/*
 *----------------------------------------------------------------------
 *
 * Keep --
 *
 *      HTTP/3 (QUIC) driver callback invoked by the NaviServer driver
 *      framework to determine whether a connection should be kept alive.
 *      In TCP-based drivers this is used to implement persistent
 *      connections, but in HTTP/3 each request stream runs over a
 *      multiplexed QUIC session managed independently of the driver.
 *
 *      This implementation is a stub that always returns NS_FALSE,
 *      since the QUIC layer itself handles session persistence and
 *      connection reuse.
 *
 * Results:
 *      Always returns NS_FALSE (no keep-alive).
 *
 * Side effects:
 *      None (only logs invocation for debugging).
 *
 *----------------------------------------------------------------------
 */
static bool
Keep(Ns_Sock *UNUSED(sock))
{
    Ns_Log(Notice, "H3 Keep");
    return NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Close --
 *
 *      HTTP/3 (QUIC) driver callback invoked by the NaviServer core to
 *      gracefully terminate a request stream. Unlike TCP-based drivers,
 *      this function does not close a physical socket - QUIC connections
 *      are multiplexed over UDP and managed by OpenSSL’s QUIC engine.
 *
 *      Instead, Close() signals that no further application data will be
 *      sent on this stream, disables read interest, and requests a final
 *      resume so the writer can emit a FIN frame once all pending data
 *      has been drained from the transmission queues.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Disables read interest via PollsetDisableRead().
 *      - Marks the shared stream buffer as closed by the application.
 *      - Optionally sets a "close_when_drained" flag under stream lock.
 *      - Triggers a final SharedRequestResume() to flush remaining data.
 *      - Enables write readiness so the event loop can deliver FIN.
 *      - Frees the per-request QuicSockCtx and detaches it from Ns_Sock.
 *
 *----------------------------------------------------------------------
 */
static void
Close(Ns_Sock *sock)
{
    NsTLSConfig *dc;
    StreamCtx   *sc;

    if (sock->driver == NULL) {
        return;
    }
    dc = sock->driver->arg;

    Ns_Log(Notice, "[%lld] H3 Close", (long long)dc->iter);

    sc = StreamCtxFromSock(dc, sock);
    if (sc == NULL || sc->ssl == NULL) {
        goto detach_sock;
    }
    Ns_Log(Notice, "[%lld] H3 Close clearing expecting_send", (long long)dc->iter);

    /* Stop reading request bytes on this stream (ok to do from producer thread). */
    PollsetDisableRead(dc, sc->ssl, sc, "Close");

    /* Mark "no more body will be enqueued" (EOF once queues drain). */
    SharedMarkClosedByApp(&sc->sh);

    /* Mark producer intent: no more body will be enqueued. */
    Ns_MutexLock(&sc->lock);
    sc->close_when_drained = NS_TRUE;          /* optional bookkeeping */
    Ns_MutexUnlock(&sc->lock);

    {
        SharedSnapshot snap = SharedSnapshotInit(&sc->sh);

        Ns_Log(Notice, "[%lld] H3[%lld] WRITER done: queued %ld pending %ld closed_by_app %d",
               (long long)dc->iter, (long long)sc->quic_sid, snap.queued_bytes, snap.pending_bytes, snap.closed_by_app);
    }

    /* Always request a resume once so the reader can emit FIN when queues empty. */
    SharedRequestResume(&sc->cc->shared, &sc->sh, sc->h3_sid);
    PollsetEnableWrite(dc, sc->ssl, sc, "Close: drain/FIN");


 detach_sock:
    /* Detach per-request sock state; lifetime of H3 objects is owned elsewhere. */
    if (sock->arg != NULL) {
        Ns_Log(Notice, "[%lld] H3 Close freeing %p", (long long)dc->iter,  (void*)sock->arg);
        ns_free(sock->arg);    /* QuicSockCtx */
        sock->arg  = NULL;
    }
    sock->sock = NS_INVALID_SOCKET; /* matches other drivers’ Close() */
}

/*
 *----------------------------------------------------------------------
 *
 * ConnInfo --
 *
 *      HTTP/3 (QUIC) driver callback used by the NaviServer framework to
 *      provide connection details as a Tcl dictionary. This function is
 *      typically invoked by ns_conn commands (e.g., ns_conn info) to
 *      expose protocol and TLS-level metadata to Tcl scripts.
 *
 *      If a valid Ns_Sock and its associated QuicSockCtx are present,
 *      the returned dictionary includes:
 *        - httpversion: "3"
 *        - sslversion:  TLS/QUIC version string (e.g., "TLSv1.3")
 *        - cipher:      negotiated cipher suite name
 *        - servername:  SNI hostname (if present)
 *        - alpn:        negotiated ALPN protocol (e.g., "h3")
 *
 * Results:
 *      Returns a newly created Tcl_DictObj* containing connection
 *      metadata. The caller becomes owner of the object reference.
 *
 * Side effects:
 *      None (purely allocates a Tcl object and reads SSL metadata).
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj*
ConnInfo(Ns_Sock *sock)
{
    Tcl_Obj *resultObj;

    resultObj = Tcl_NewDictObj();

    if (sock != NULL && sock->arg != NULL) {
        QuicSockCtx *qctx = sock->arg;

        if (qctx->is_h3) {
            Tcl_DictObjPut(NULL, resultObj,
                           Tcl_NewStringObj("httpversion", sizeof("httpversion")-1),
                           Tcl_NewStringObj("3", 1));
        }
        if (qctx->ssl != NULL) {
            Tcl_DictObjPut(NULL, resultObj,
                           Tcl_NewStringObj("sslversion", 10),
                           Tcl_NewStringObj(SSL_get_version(qctx->ssl), TCL_INDEX_NONE));
            Tcl_DictObjPut(NULL, resultObj,
                           Tcl_NewStringObj("cipher", 6),
                           Tcl_NewStringObj(SSL_get_cipher(qctx->ssl), TCL_INDEX_NONE));
            Tcl_DictObjPut(NULL, resultObj,
                           Tcl_NewStringObj("servername", 10),
                           Tcl_NewStringObj(SSL_get_servername(qctx->ssl, TLSEXT_NAMETYPE_host_name), TCL_INDEX_NONE));
            {
                const char  *alpnString;
                unsigned int alpnLength;

                SSL_get0_alpn_selected(qctx->ssl, (const unsigned char **)&alpnString, &alpnLength);
                Tcl_DictObjPut(NULL, resultObj,
                               Tcl_NewStringObj("alpn", 4),
                               Tcl_NewStringObj(alpnString, (TCL_SIZE_T)alpnLength));
            }
        }
    }

    return resultObj;
}

#else
NS_EXPORT Ns_ReturnCode Ns_ModuleInit(const char *server, const char *module) {
    Ns_Log(Warning, "OpenSSL 4+ and nghttp3 are needed to load this module");
    return NS_ERROR;
}

#endif /* OPENSSL_VERSION_NUMBER >= 0x40000000L */
#endif
/* End: HAVE_OPENSSL_EVP_H: Big ifdef block that covers most of this file. */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
