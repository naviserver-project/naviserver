/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
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
# endif

# if !defined(HAVE_OPENSSL_PRE_1_1) && !defined(LIBRESSL_VERSION_NUMBER)
#  define HAVE_OPENSSL_HKDF
#  define HAVE_OPENSSL_EC_PRIV2OCT
# endif

# include <openssl/ssl.h>
# include <openssl/err.h>

typedef struct NsSSLConfig {
    SSL_CTX  *ctx;
    Ns_Mutex  lock;
    int       verify;
    int       deferaccept;  /* Enable the TCP_DEFER_ACCEPT optimization. */
    int       nodelay;      /* Enable the TCP_NODELAY optimization. */
    DH       *dhKey512;     /* Fallback Diffie Hellman keys of length 512 */
    DH       *dhKey1024;    /* Fallback Diffie Hellman keys of length 1024 */
    DH       *dhKey2048;    /* Fallback Diffie Hellman keys of length 2048 */
} NsSSLConfig;

NS_EXTERN NsSSLConfig *NsSSLConfigNew(const char *path)
   NS_GNUC_NONNULL(1);

#endif
