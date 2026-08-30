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


#ifdef HAVE_OPENSSL_EVP_H

/*
 * Common definitions of handling versions of openssl/libressl
 */

#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x10101000L
# define HAVE_OPENSSL_PRE_1_1_1
#endif

# if defined(LIBRESSL_VERSION_NUMBER)
#  if LIBRESSL_VERSION_NUMBER >= 0x2060300fL && LIBRESSL_VERSION_NUMBER < 0x20700000L
#   define LIBRESSL_1_0_2
#  endif
# endif

# if defined(OPENSSL_VERSION_MAJOR) && OPENSSL_VERSION_MAJOR >= 3
#  define HAVE_OPENSSL_3
#  if OPENSSL_VERSION_PREREQ(3,2)
#   define HAVE_OPENSSL_3_2
#  endif
#  if OPENSSL_VERSION_PREREQ(3,5)
#   define HAVE_OPENSSL_3_5
#  endif
#  if OPENSSL_VERSION_PREREQ(4,0)
#   define HAVE_OPENSSL_4
#   define HAVE_OPENSSL_4_0
#  endif
#  if OPENSSL_VERSION_PREREQ(4,1) || (OPENSSL_VERSION_MAJOR == 4 && OPENSSL_VERSION_MINOR == 0 && OPENSSL_VERSION_PATCH >= 2)
#   define HAVE_OPENSSL_4_0_2
#  endif
#  if OPENSSL_VERSION_PREREQ(4,1)
#   define HAVE_OPENSSL_4_1
#  endif
# endif

# ifndef LIBRESSL_VERSION_NUMBER
#  define HAVE_OPENSSL_HKDF
#  define HAVE_OPENSSL_EC_PRIV2OCT
# endif

# if defined(HAVE_OPENSSL_3)
#  define HAVE_OPENSSL_OCSP
# endif

# include <openssl/ssl.h>
# include <openssl/err.h>
#include "nsatomic.h"

#if defined(HAVE_OPENSSL_4)
typedef struct NsTLSH3Config {
    size_t      recvbufsize;
    size_t      nr_listeners;
    int         cc_idx;
    int         sc_idx;
    size_t      first_dead;

    /*
     * TLS/QUIC waker: works around SSL_poll not supporting an
     * external trigger descriptor.
     */
    struct sockaddr_storage waker_addr;
    socklen_t               waker_addrlen;
    NS_SOCKET               waker_fd;

    Ns_AtomicUint32         waker_pending;
    uint64_t                progress_epoch;

    /*
     * Pollset storage. All three arrays have poll_capacity entries.
     */
    SSL_POLL_ITEM *poll_items;
    size_t         npoll;
    size_t         poll_capacity;

    /*
     * ssl_items, mutex_items, and shared_mutex_items are parallel to
     * poll_items. dead_items and conns are independent lists.
     */
    Ns_DList ssl_items;
    Ns_DList mutex_items;
    Ns_DList shared_mutex_items;
    Ns_DList dead_items;
    Ns_DList conns;

    struct timeval idle_timeout;
    struct timeval drain_timeout;
    bool validate_client_address;
    bool reuseport;
} NsTLSH3Config;
#endif


typedef struct NsTLSConfig {
    Ns_Driver  *driver; /* Default context for driver                   */
    SSL_CTX    *ctx;
    uint64_t    iter;
    Ns_TLSClientCertMode clientCertMode;
    const char *tlsKeyScript;
    const char *tlsKeylogFile;
    const char *vhostcertificates;
    int         sni_idx;
    union {
        struct {
            int    deferaccept;       /* Enable the TCP_DEFER_ACCEPT optimization.         */
            int    nodelay;           /* Enable the TCP_NODELAY optimization.              */
            bool   h3advertise;       /* add h3 advertise automatically when h3 is enabled */
            bool   h3persist;         /* add persit flag to h3 advertise when activated    */
        } h1;
# if defined(HAVE_OPENSSL_4)
        NsTLSH3Config h3;
# endif
    } u;
} NsTLSConfig;

NS_EXTERN NsTLSConfig *NsTLSConfigNew(const char *section)
   NS_GNUC_NONNULL(1);

#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
