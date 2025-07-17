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

/*
 * Some OPENSSL_VERSION_NUMBERs:
 *    0x10101005L OpenSSL 1.1.1-pre5 (beta)
 *    0x1010007fL OpenSSL 1.1.0g
 *    0x1000200fL OpenSSL 1.0.2-fips
 *
 * LibreSSL
 *    OPENSSL_VERSION_NUMBER 0x20000000L + LIBRESSL_VERSION_NUMBER 0x2090200fL
 */

/*
 * OpenSSL < 0.9.8f does not have SSL_set_tlsext_host_name() In some
 * versions, this function is defined as a macro, on some versions as
 * a library call, which complicates detection via m4
 */

# if OPENSSL_VERSION_NUMBER > 0x00908070
#  define HAVE_SSL_set_tlsext_host_name 1
# endif

# if defined(LIBRESSL_VERSION_NUMBER)
#  if LIBRESSL_VERSION_NUMBER >= 0x2060300fL && LIBRESSL_VERSION_NUMBER < 0x20700000L
#   define LIBRESSL_1_0_2
#  endif
#  if LIBRESSL_VERSION_NUMBER >= 0x20700000L && LIBRESSL_VERSION_NUMBER < 0x2090000fL
#   define LIBRESSL_2_7
#  endif
#  if LIBRESSL_VERSION_NUMBER >= 0x2090000fL
#   define LIBRESSL_2_9_0
#  endif
# endif

# if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_1_0_2)
#  define HAVE_OPENSSL_PRE_1_1
# endif

# if OPENSSL_VERSION_NUMBER < 0x10002000L
#  define HAVE_OPENSSL_PRE_1_0_2
# endif

# if OPENSSL_VERSION_NUMBER < 0x10000000L
#  define HAVE_OPENSSL_PRE_1_0
# endif

# if defined(OPENSSL_VERSION_MAJOR) && OPENSSL_VERSION_MAJOR >= 3
#  define HAVE_OPENSSL_3 1
#  if OPENSSL_VERSION_PREREQ(3,2)
#   define HAVE_OPENSSL_3_2 1
#  endif
#  if OPENSSL_VERSION_PREREQ(3,5)
#   define HAVE_OPENSSL_3_5 1
#  endif
# endif

# if !defined(HAVE_OPENSSL_PRE_1_1) && !defined(LIBRESSL_VERSION_NUMBER)
#  define HAVE_OPENSSL_HKDF
#  define HAVE_OPENSSL_EC_PRIV2OCT
# endif

# include <openssl/ssl.h>
# include <openssl/err.h>

typedef struct NsSSLConfig {
    SSL_CTX    *ctx;
    Ns_Mutex    lock;
    const char *tlsKeyScript;
    int         verify;
    int         deferaccept;  /* Enable the TCP_DEFER_ACCEPT optimization. */
    int         nodelay;      /* Enable the TCP_NODELAY optimization. */
    DH         *dhKey512;     /* Fallback Diffie Hellman keys of length 512 */
    DH         *dhKey1024;    /* Fallback Diffie Hellman keys of length 1024 */
    DH         *dhKey2048;    /* Fallback Diffie Hellman keys of length 2048 */
} NsSSLConfig;

NS_EXTERN NsSSLConfig *NsSSLConfigNew(const char *section)
   NS_GNUC_NONNULL(1);

#endif
