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

/*
 * tls.c --
 *
 *      Support for OpenSSL support (SSL and TLS), mostly for HTTPS
 */

#include "nsd.h"

static void ReportError(Tcl_Interp *interp, const char *fmt, ...)
    NS_GNUC_NONNULL(2) NS_GNUC_PRINTF(2,3);

/*
 *----------------------------------------------------------------------
 *
 * ReportError --
 *
 *      Error reporting function for this file.  The function has the
 *      fprintf interface (format string plus arguments) and leaves an
 *      error message in the interpreter's result object, when an
 *      interpreter is provided. Otherwise, it outputs a warning to
 *      the system log.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Error reporting.
 *
 *----------------------------------------------------------------------
 */
static void
ReportError(Tcl_Interp *interp, const char *fmt, ...)
{
    va_list     ap;
    Tcl_DString ds;

    NS_NONNULL_ASSERT(fmt != NULL);

    Tcl_DStringInit(&ds);
    va_start(ap, fmt);
    Ns_DStringVPrintf(&ds, fmt, ap);
    va_end(ap);
    if (interp != NULL) {
        Tcl_DStringResult(interp, &ds);
    } else {
        Ns_Log(Warning, "%s", ds.string);
        Tcl_DStringFree(&ds);
    }
}

#ifdef HAVE_OPENSSL_EVP_H
# include "nsopenssl.h"
# include <openssl/ssl.h>
# include <openssl/err.h>

/*
 * OpenSSL < 0.9.8f does not have SSL_set_tlsext_host_name() In some
 * versions, this function is defined as a macro, on some versions as
 * a library call, which complicates detection via m4.
 */
# if OPENSSL_VERSION_NUMBER > 0x00908070
#  define HAVE_SSL_set_tlsext_host_name 1
# endif

# ifndef HAVE_OPENSSL_PRE_1_1
#  define OPENSSL_HAVE_DH_AUTO
#  define OPENSSL_HAVE_READ_BUFFER_LEN
# endif

# ifndef HAVE_X509_STORE_CTX_GET_OBJ_BY_SUBJECT
#  define OPENSSL_NO_OCSP 1
# endif

# ifndef OPENSSL_NO_OCSP
#  include <openssl/ocsp.h>
# endif

# ifndef OPENSSL_NO_OCSP
/*
 * Structure passed to cert status callback
 */
typedef struct {
    int            timeout;
    //char          *respin;     /* File to load OCSP Response from (or NULL if no file) */
    int            verbose;
    OCSP_RESPONSE *OCSPresp;
    Ns_Time        OCSPexpire;
    Ns_Time        OCSPcheckInterval;
} SSLCertStatusArg;

static SSLCertStatusArg sslCertStatusArg;
# endif

# if OPENSSL_VERSION_NUMBER >= 0x10101000L
static FILE *keylog_fp = NULL;
# endif

/*
 * For HTTP client requests, use a data index to obtain server
 * information from an SSL_CTX.
 */
static int ClientCtxDataIndex;

static char *FilenameToEnvVar(Tcl_DString *dsPtr, const char *filename)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ReturnCode BuildALPNWireFormat(Tcl_DString *dsPtr, const char *alpnStr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * OpenSSL callback functions.
 */
static int SSL_serverNameCB(SSL *ssl, int *al, void *arg);
static int TLSPasswordCB(char *buf, int size, int rwflag, void *userdata);
static int ALPNSelectCB(NS_TLS_SSL *UNUSED(ssl), const unsigned char **out, unsigned char *outlen,
                        const unsigned char *clientProtos, unsigned int clientProtosLength, void *arg);
# if OPENSSL_VERSION_NUMBER >= 0x10101000L
static void KeylogCB(const SSL *ssl, const char *line);
# endif

# ifdef HAVE_OPENSSL_PRE_1_1
static void SSL_infoCB(const SSL *ssl, int where, int ret);
# endif
static int CertficateValidationCB(int preverify_ok, X509_STORE_CTX *ctx);

static Ns_ReturnCode StoreInvalidCertificate(X509 *cert, int x509err, int currentDepth, NsServer *servPtr)
    NS_GNUC_NONNULL(4);
static bool ValidationExcpetionExists(int x509err, NS_SOCKET sock, Ns_DList *validationExceptionsPtr, struct sockaddr *saPtr)
     NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

#ifndef OPENSSL_HAVE_DH_AUTO
static DH *SSL_dhCB(SSL *ssl, int isExport, int keyLength);
#endif

# ifndef OPENSSL_NO_OCSP
static int SSL_cert_statusCB(SSL *ssl, void *arg);

/*
 * Local functions defined in this file
 */
static Ns_ReturnCode
PartialTimeout(const Ns_Time *endTimePtr, Ns_Time *diffPtr, Ns_Time *defaultPartialTimeoutPtr,
               Ns_Time **partialTimeoutPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static bool
OCSP_ResponseIsValid(OCSP_RESPONSE *resp, OCSP_CERTID *id)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
# endif

static void DrainErrorStack(Ns_LogSeverity severity, const char *errorContext, unsigned long sslERRcode)
    NS_GNUC_NONNULL(2);

static Ns_ReturnCode WaitFor(NS_SOCKET sock, unsigned int st, const Ns_Time *timeoutPtr);

static void CertTableInit(void);
static void CertTableReload(void *UNUSED(arg));
static void CertTableAdd(const NS_TLS_SSL_CTX *ctx, const char *cert)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static NS_TLS_SSL_CTX *CertTableGetCtx(const char *cert)
    NS_GNUC_NONNULL(1);


# ifndef OPENSSL_NO_OCSP
static int OCSP_FromCacheFile(Tcl_DString *dsPtr, OCSP_CERTID *id, OCSP_RESPONSE **resp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static OCSP_CERTID *OCSP_get_cert_id(const SSL *ssl, X509 *cert, bool *selfSignedPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int OCSP_computeResponse(SSL *ssl, const SSLCertStatusArg *srctx, OCSP_RESPONSE **resp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static OCSP_RESPONSE *OCSP_FromAIA(OCSP_REQUEST *req, const char *aiaURL, int req_timeout)
    NS_GNUC_NONNULL(1);
#endif

# if !defined(HAVE_OPENSSL_PRE_1_1) && !defined(LIBRESSL_VERSION_NUMBER)
static void *NS_CRYPTO_malloc(size_t num, const char *UNUSED(file), int UNUSED(line)) NS_GNUC_MALLOC NS_ALLOC_SIZE1(1) NS_GNUC_RETURNS_NONNULL;
static void *NS_CRYPTO_realloc(void *addr, size_t num, const char *UNUSED(file), int UNUSED(line)) NS_ALLOC_SIZE1(2);
static void NS_CRYPTO_free(void *addr, const char *UNUSED(file), int UNUSED(line));
# endif

#ifndef OPENSSL_HAVE_DH_AUTO
/*
 *----------------------------------------------------------------------
 *
 * Include pre-generated DH parameters
 *
 *----------------------------------------------------------------------
 */
# ifndef HEADER_DH_H
#  include <openssl/dh.h>
# endif
static DH *get_dh512(void);
static DH *get_dh1024(void);

# if  defined(HAVE_OPENSSL_PRE_1_1) || defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x20700000L
/*
 *----------------------------------------------------------------------
 *
 * DH_set0_pqg --
 *
 *      Compatibility function for libressl < 2.7; DH_set0_pqg is used
 *      just by the Diffie-Hellman parameters in dhparams.h. Obsoleted
 *      by OPENSSL_HAVE_DH_AUTO.
 *
 * Results:
 *      Returns walways 1.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DH_set0_pqg(DH *dh, BIGNUM *p, BIGNUM *q, BIGNUM *g)
{
    if ((dh->p == NULL && p == NULL) || (dh->g == NULL && g == NULL)) {
        return 0;
    }
    if (p != NULL) {
        BN_free(dh->p);
        dh->p = p;
    }
    if (q != NULL) {
        BN_free(dh->q);
        dh->q = q;
    }
    if (g != NULL) {
        BN_free(dh->g);
        dh->g = g;
    }
    return 1;
}
# endif /* LIBRESSL_VERSION_NUMBER */

#include "dhparams.h"

/*
 *----------------------------------------------------------------------
 * Callback implementations.
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * SSL_dhCB --
 *
 *      OpenSSL callback used for ephemeral DH keys.
 *      Obsoleted by OPENSSL_HAVE_DH_AUTO.
 *
 * Results:
 *      Returns always 1.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static DH *
SSL_dhCB(SSL *ssl, int isExport, int keyLength) {
    NsSSLConfig *cfgPtr;
    DH          *key;
    SSL_CTX     *ctx;

    NS_NONNULL_ASSERT(ssl != NULL);

    ctx = SSL_get_SSL_CTX(ssl);

    Ns_Log(Debug, "SSL_dhCB: isExport %d keyLength %d", isExport, keyLength);
    cfgPtr = (NsSSLConfig *)SSL_CTX_get_app_data(ctx);
    assert(cfgPtr != NULL);

    switch (keyLength) {
    case 512:
        key = cfgPtr->dhKey512;
        break;

    case 1024:
        key = cfgPtr->dhKey1024;
        break;

    case 2048: NS_FALL_THROUGH; /* fall through */
    default:
        key = cfgPtr->dhKey2048;
        break;
    }
    Ns_Log(Debug, "SSL_dhCB: returns %p\n", (void *)key);
    return key;
}
#endif /* OPENSSL_HAVE_DH_AUTO */


#ifdef HAVE_OPENSSL_PRE_1_1
/*
 *----------------------------------------------------------------------
 *
 * SSL_infoCB --
 *
 *      OpenSSL callback used for renegotiation in earlier versions of
 *      OpenSSL.  The renegotiation issue was fixed in recent versions
 *      of OpenSSL, and the flag was removed, therefore, this function
 *      is just for compatibility with old version of OpenSSL (flag
 *      removed in OpenSSL 1.1.*).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SSL_infoCB(const SSL *ssl, int where, int UNUSED(ret)) {

    NS_NONNULL_ASSERT(ssl != NULL);

    if ((where & SSL_CB_HANDSHAKE_DONE)) {
        ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
    }
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * SSL_serverNameCB --
 *
 *      OpenSSL callback used for server-ide server name indication
 *      (SNI). The callback is controlled by the driver option
 *      NS_DRIVER_SNI.
 *
 * Results:
 *      SSL_TLSEXT_ERR_NOACK or SSL_TLSEXT_ERR_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
/*
 * ServerNameCallback for SNI
 */
static int
SSL_serverNameCB(SSL *ssl, int *al, void *UNUSED(arg))
{
    const char  *serverName;
    int          result = SSL_TLSEXT_ERR_NOACK;

    NS_NONNULL_ASSERT(ssl != NULL);

    serverName = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

    if (serverName != NULL) {
        Ns_Sock  *sockPtr = (Ns_Sock*)SSL_get_app_data(ssl);
        Driver   *drvPtr;
        bool      doSNI;

        assert(sockPtr != NULL);

        drvPtr = (Driver *)(sockPtr->driver);
        doSNI = ((drvPtr->opts & NS_DRIVER_SNI) != 0u);

        /*
         * The default for *al is initialized by SSL_AD_UNRECOGNIZED_NAME = 112.
         * Find info about these codes via:
         *    fgrep -r --include=*.h 112 /usr/local/src/openssl/ | fgrep AD
         */
        Ns_Log(Debug, "SSL_serverNameCB got server name <%s> al %d doSNI %d",
               serverName, (al != NULL ? *al : 0), doSNI);

        /*
         * Perform lookup from host table only, when doSNI is true
         * (i.e., when per virtual server certificates were specified.
         */
        if (doSNI) {
            Tcl_DString     ds;
            unsigned short  port = Ns_SockGetPort(sockPtr);
            NS_TLS_SSL_CTX *ctx;

            /*
             * The virtual host entries are specified canonically, via
             * host:port. Since the provided "serverName" is specified by the
             * client, we can't precompute the strings to save a few cycles.
             */
            Tcl_DStringInit(&ds);
            Ns_DStringPrintf(&ds, "%s:%hu", serverName, port);

            ctx = NsDriverLookupHostCtx(&ds, serverName, sockPtr->driver);

            Ns_Log(Debug, "SSL_serverNameCB lookup result of <%s> location %s port %hu -> ctx %p",
                   serverName, ds.string, port, (void*)ctx);

            /*
             * When the lookup succeeds, we have the alternate SSL_CTX
             * that we will use. Otherwise, do not acknowledge the
             * servername request.  Return the same value as when not
             * servername was provided (SSL_TLSEXT_ERR_NOACK).
             */
            if (ctx != NULL) {
                Ns_Log(Debug, "SSL_serverNameCB switches server context");
                SSL_set_SSL_CTX(ssl, ctx);
                result = SSL_TLSEXT_ERR_OK;
            }
            Tcl_DStringFree(&ds);
        }
    }

    return result;
}

#ifndef OPENSSL_NO_OCSP

/*
 *----------------------------------------------------------------------
 *
 * SSL_cert_has_must_staple --
 *
 *      Check whether an X.509 certificate has the “must-staple” TLS
 *      Feature extension (OCSP Must-Staple, OID 1.3.6.1.5.5.7.1.24)
 *      indicating that the certificate requires OCSP stapling.
 *
 * Parameters:
 *      cert    - pointer to the X509 certificate to inspect
 *
 * Returns:
 *      1  if the TLS Feature extension is present and includes the
 *         status_request feature (value 5), indicating Must-Staple.
 *      0  if the extension is absent or present but does not include
 *         the status_request feature.
 *     -1  on error (e.g. OID lookup or extension parsing failure).
 *
 * Side Effects:
 *      Logs a warning if the TLS Feature extension is present but
 *      cannot be parsed.
 *
 *----------------------------------------------------------------------
 */
static int SSL_cert_has_must_staple(X509 *cert) {
    int ext_index;
    ASN1_OBJECT *obj = OBJ_txt2obj("1.3.6.1.5.5.7.1.24", 1);  // TLS Feature OID

    if (obj == NULL) {
        return -1;
    }
    ext_index = X509_get_ext_by_OBJ(cert, obj, -1);
    ASN1_OBJECT_free(obj);

    if (ext_index < 0) {
        /*
         * TLS Feature extension not found
         */
        return 0;
    } else {
        X509_EXTENSION      *ext = X509_get_ext(cert, ext_index);
        ASN1_OCTET_STRING   *octet = X509_EXTENSION_get_data(ext);
        const unsigned char *p = ASN1_STRING_get0_data(octet);
        long                 len = ASN1_STRING_length(octet);
        STACK_OF(ASN1_TYPE) *features = d2i_ASN1_SEQUENCE_ANY(NULL, &p, len);

        if (!features) {
            Ns_Log(Warning, "OCSP: Failed to parse TLS Feature extension");
            return -1;
        }

        for (int i = 0; i < sk_ASN1_TYPE_num(features); i++) {
            ASN1_TYPE *type = sk_ASN1_TYPE_value(features, i);
            if (type->type == V_ASN1_INTEGER) {
                ASN1_INTEGER *feature = type->value.integer;
                long val = ASN1_INTEGER_get(feature);
                if (val == 5) { // 5 = status_request (i.e., Must-Staple)
                    sk_ASN1_TYPE_pop_free(features, ASN1_TYPE_free);
                    return 1;
                }
            }
        }
        sk_ASN1_TYPE_pop_free(features, ASN1_TYPE_free);
    }

    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * openssl_string_free --
 *
 *      Helper functionto free strings allocated by OpenSSL stack APIs.
 *
 * Parameters:
 *      string - pointer to the string to free
 *
 * Side Effects:
 *      Calls OPENSSL_free() on the given pointer.
 *
 *----------------------------------------------------------------------
 */
static void openssl_string_free(char *chars)
{
    OPENSSL_free(chars);
}

/*
 *----------------------------------------------------------------------
 *
 * SSL_cert_can_staple --
 *
 *      Determine whether an X.509 certificate supports OCSP stapling
 *      by checking for the presence of one or more OCSP responder URLs
 *      in its Authority Information Access (AIA) extension.
 *
 * Parameters:
 *      cert    - pointer to the X509 certificate to inspect (may be NULL)
 *
 * Returns:
 *      true  if the certificate contains at least one OCSP URI and thus
 *            can staple OCSP responses;
 *      false if cert is NULL or if no OCSP URI is found.
 *
 * Side Effects:
 *      Calls X509_get1_ocsp() to extract a STACK_OF(OPENSSL_STRING),
 *      and then frees that stack and its strings via sk_OPENSSL_STRING_pop_free()
 *      with openssl_free() as the deallocator.
 *
 *----------------------------------------------------------------------
 */
static bool SSL_cert_can_staple(X509 *cert) {
    bool success = NS_FALSE;

    if (cert != NULL) {
        /*
         * Get the stack of OCSP URIs from the AIA extension
         */
        STACK_OF(OPENSSL_STRING) *ocsp_uris = X509_get1_ocsp(cert);
        if (ocsp_uris != NULL) {
            /*
             * We have an OCSP URI, we can staple
             */
            success = NS_TRUE;
            sk_OPENSSL_STRING_pop_free(ocsp_uris, openssl_string_free);
        }
    }
    return success;
}

static int SSL_cert_statusCB(SSL *ssl, void *arg)
{
    SSLCertStatusArg *srctx = arg;
    int               result = SSL_TLSEXT_ERR_ALERT_FATAL;
    OCSP_RESPONSE    *resp = NULL;
    unsigned char    *rspder = NULL;
    int               rspderlen;
    bool              muststaple, canstaple;
    Ns_Time           now, diff;
    X509             *cert;

    if (srctx->verbose) {
        Ns_Log(Notice, "cert_status: callback called");
    }

    cert = SSL_get_certificate(ssl);
    if (cert == NULL) {
        /*
         * No certificate available
         */
        return SSL_TLSEXT_ERR_NOACK;
    }
    muststaple = (SSL_cert_has_must_staple(cert) == 1);
    canstaple = (SSL_cert_can_staple(cert) == 1);

    Ns_Log(Debug, "tls: SSL_cert_statusCB must staple %d, can staple %d", muststaple, canstaple);
    if (!canstaple) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    Ns_GetTime(&now);
    /*
     * If there is a in-memory changed OCSP response, validate this if
     * necessary.
     */
    if (srctx->OCSPresp != NULL) {

        if (Ns_DiffTime(&srctx->OCSPexpire, &now, &diff) < 0) {
            OCSP_CERTID *cert_id;
            bool         flush;

            Ns_Log(Notice, "cert_status: must validate OCSP response " NS_TIME_FMT " sec",
                    (int64_t) diff.sec, diff.usec);

            cert_id = OCSP_get_cert_id(ssl, SSL_get_certificate(ssl), NULL);
            if (cert_id == NULL) {
                Ns_Log(Warning, "cert_status: OCSP validation: certificate id is unknown");
                flush = NS_TRUE;
            } else {
                Ns_Log(Debug, "cert_status: CAN VALIDATE OCSP response");
                flush = !OCSP_ResponseIsValid(srctx->OCSPresp, cert_id);
            }
            if (flush) {
                Ns_Log(Debug, "cert_status: flush OCSP response");
                OCSP_RESPONSE_free(srctx->OCSPresp);
                srctx->OCSPresp = NULL;
            } else {
                now.sec += srctx->OCSPcheckInterval.sec;
                srctx->OCSPexpire = now;
            }
        } else {
            Ns_Log(Debug, "cert_status: RECENT OCSP response, recheck in " NS_TIME_FMT " sec",
                    (int64_t) diff.sec, diff.usec);
        }
    }

    /*
     * If we have no in-memory cached OCSP response, fetch the value
     * either from the disk cache or from the URL provided via the DER
     * encoded OCSP request.
     *
     * In failure cases, avoid a too eager generation of error
     * messages in the logfile by performing also retries to obtain the
     * OCSP response based on the timeout.
     */

    if (srctx->OCSPresp == NULL
        && ((srctx->OCSPexpire.sec == 0) || Ns_DiffTime(&srctx->OCSPexpire, &now, &diff) < 0)
        ) {

        result = OCSP_computeResponse(ssl, srctx, &resp);
        /*
         * Sometimes a firewall blocks the access to an AIA server. In
         * this case, we cannot provide a stapling. In such cases,
         * behave just like without OCSP stapling.
         */
        if (unlikely(resp == NULL)) {
            /*
             * We got no response.
             */
            Ns_Log(Debug, "We got no response, result %d", result);
        } else {
            /*
             * We got a response.
             */
            if (result != SSL_TLSEXT_ERR_OK) {
                OCSP_RESPONSE_free(resp);
                goto err;
            }
            /*
             * Perform in-memory caching of the OCSP_RESPONSE.
             */
            srctx->OCSPresp = resp;
            /*
             * Avoid Ns_GetTime() on every invocation.
             */
        }
        Ns_GetTime(&now);
        now.sec +=  srctx->OCSPcheckInterval.sec;
        srctx->OCSPexpire = now;
    } else {
        resp = srctx->OCSPresp;
    }

    if (resp != NULL) {
        rspderlen = i2d_OCSP_RESPONSE(resp, &rspder);

        Ns_Log(Debug, "cert_status: callback returns OCSP_RESPONSE with length %d", rspderlen);
        if (rspderlen <= 0) {
            if (resp != NULL) {
                OCSP_RESPONSE_free(resp);
                srctx->OCSPresp = NULL;
            }
            goto err;
        }
        SSL_set_tlsext_status_ocsp_resp(ssl, rspder, rspderlen);
        if (srctx->verbose) {
            Ns_Log(Notice, "cert_status: OCSP response sent to client");
            //OCSP_RESPONSE_print(bio_err, resp, 2);
        }
        result = SSL_TLSEXT_ERR_OK;
    } else {
        /*
         * We could not find a cached result or a response from the
         * AIA. We cannot perform staping, but we still do not want to
         * cancel the request fully. We have to cancel the previous
         * error from the OCSP validity check, otherwise OpenSSL will
         * cancel the request with:
         * "routines:OCSP_check_validity:status expired"
         */
        result = SSL_TLSEXT_ERR_NOACK;

        ERR_clear_error();

        Ns_Log(Notice, "cert_status: OCSP cannot validate the certificate");
    }

 err:
    if (result != SSL_TLSEXT_ERR_OK) {
        //ERR_print_errors(bio_err);
    }

    //OCSP_RESPONSE_free(resp);
    Ns_Log(Debug, "SSL_cert_statusCB returns result %d", result);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * OCSP_get_cert_id --
 *
 *      Extract OCSP_CERTID from the provided certificate via the OpenSSL
 *      certificate store. This function requires a properly initialiized
 *      certificate store containing the full certificate chain.
 *
 * Results:
 *      OCSP_CERTID pointer or NULL in cases of failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static OCSP_CERTID *
OCSP_get_cert_id(const SSL *ssl, X509 *cert, bool *selfSignedPtr)
{
    X509_STORE_CTX *store_ctx;
    OCSP_CERTID    *result = NULL;

    NS_NONNULL_ASSERT(ssl != NULL);
    NS_NONNULL_ASSERT(cert != NULL);

    store_ctx = X509_STORE_CTX_new();
    if (store_ctx == NULL
        || (!X509_STORE_CTX_init(store_ctx,
                                 SSL_CTX_get_cert_store(SSL_get_SSL_CTX(ssl)),
                                 NULL, NULL))) {
        Ns_Log(Error, "cert_status: cannot initialize certificate storage context");

    } else {
        X509        *issuer;
        X509_OBJECT *x509_obj;
        int          rc = X509_STORE_CTX_get1_issuer(&issuer, store_ctx, cert);

        if (rc == -1) {
            Ns_Log(Warning, "cert_status: can't retrieve issuer of certificate");

        } else if (rc == 0) {
            Ns_Log(Warning, "cert_status: OCSP stapling ignored, "
                   "issuer certificate not found");
        }
        x509_obj = X509_STORE_CTX_get_obj_by_subject(store_ctx, X509_LU_X509,
                                            X509_get_issuer_name(cert));
        if (x509_obj == NULL) {
            Ns_Log(Warning, "cert_status: Can't retrieve issuer certificate");

        } else {
            result = OCSP_cert_to_id(NULL, cert, X509_OBJECT_get0_X509(x509_obj));
            X509_OBJECT_free(x509_obj);
        }
        if (selfSignedPtr != NULL) {
            *selfSignedPtr = (X509_NAME_hash_ex(X509_get_issuer_name(cert), NULL, NULL, NULL)
                              == X509_NAME_hash_ex(X509_get_subject_name(cert), NULL, NULL, NULL));
        }
    }

    if (store_ctx != NULL) {
        X509_STORE_CTX_free(store_ctx);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * OCSP_ResponseIsValid --
 *
 *      Check the validity of a OCSP response. This test includes
 *      checks of expiration dates.
 *
 * Results:
 *      Boolean value.
 *
 * Side effects:
 *      In case of failures of validity checks, warnings are written
 *      to the system log.
 *
 *----------------------------------------------------------------------
 */
static bool
OCSP_ResponseIsValid(OCSP_RESPONSE *resp, OCSP_CERTID *id)
{
    int                   n;
    bool                  result;
    OCSP_BASICRESP       *basic;
    ASN1_GENERALIZEDTIME *thisupdate, *nextupdate;

    NS_NONNULL_ASSERT(resp != NULL);
    NS_NONNULL_ASSERT(id != NULL);

    basic = OCSP_response_get1_basic(resp);
    if (basic == NULL) {
        Ns_Log(Warning, "OCSP cache file is invalid (no basic)");
        result = NS_FALSE;
    } else if (OCSP_resp_find_status(basic, id, &n, NULL, NULL,
                                     &thisupdate, &nextupdate) != 1) {
        Ns_Log(Warning, "OCSP cache file is invalid (no dates)");
        result = NS_FALSE;
    } else {
        result = (OCSP_check_validity(thisupdate, nextupdate,
                                      300 /*MAX_VALIDITY_PERIOD*/,
                                      -1 /* status_age, additional check
                                            not performed.*/) == 1);
        Ns_Log(Notice, "OCSP cache file is valid (expiry): %d", result);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * OCSP_FromCacheFile --
 *
 *      Try to load OCSP_RESPONSE from cache file.
 *
 * Results:
 *      A standard Tcl result (TCL_OK, TCL_CONTINUE, TCL_ERROR).
 *      TCL_CONTINUE means that there is no cache entry yet, but the
 *      filename of the cache file is returned in the first argument.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
OCSP_FromCacheFile(Tcl_DString *dsPtr, OCSP_CERTID *id, OCSP_RESPONSE **resp)
{
    ASN1_INTEGER *pserial = NULL;
    int           result = TCL_ERROR;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(id != NULL);
    NS_NONNULL_ASSERT(resp != NULL);

    if (OCSP_id_get0_info(NULL, NULL, NULL, &pserial, id) != 0) {
        Tcl_DString outputBuffer;
        struct stat fileInfo;
        const char *fileName;

        Tcl_DStringInit(&outputBuffer);
        Tcl_DStringSetLength(&outputBuffer, (TCL_SIZE_T)(pserial->length*2 + 1));

        Ns_HexString(pserial->data, outputBuffer.string, (TCL_SIZE_T)pserial->length, NS_TRUE);

        /*
         * A result of TCL_CONTINUE or TCL_OK implies a computed filename
         * of the cache file in dsPtr;
         */
        Tcl_DStringAppend(&outputBuffer, ".der", 4);
        fileName = Ns_MakePath(dsPtr, nsconf.logDir, outputBuffer.string, NS_SENTINEL);
        result = TCL_CONTINUE;

        if (Ns_Stat(dsPtr->string, &fileInfo)) {
            BIO *derbio;

            /*fprintf(stderr, "... file %s exists (%ld bytes)\n",
              fileName, (long)fileInfo.st_size);*/
            Ns_Log(Notice, "OCSP cache file exists: %s", fileName);

            derbio = BIO_new_file(fileName, "rb");
            if (derbio == NULL) {
                Ns_Log(Warning, "cert_status: Cannot open OCSP response file: %s", fileName);

            } else {

                *resp = d2i_OCSP_RESPONSE_bio(derbio, NULL);
                BIO_free(derbio);

                if (*resp == NULL) {
                    Ns_Log(Warning, "cert_status: Error reading OCSP response file: %s", fileName);
                } else if (OCSP_ResponseIsValid(*resp, id)) {
                    result = TCL_OK;
                }
            }
        } else {
            Ns_Log(Warning, "OCSP cache file does not exist: %s", fileName);
            result = TCL_CONTINUE;
        }
        Tcl_DStringFree(&outputBuffer);

    } else {
        Ns_Log(Warning, "cert_status: cannot obtain Serial Number from certificate");

    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * OCSP_computeResponse --
 *
 *      Get OCSP_RESPONSE either from a cache file or from the certificate
 *      issuing server via the DER encoded OCSP request. In case the disk
 *      lookup fails, but the request to the AIA server succeeds, the result
 *      is stored for caching in the filesystem.
 *
 * Results:
 *      OpenSSL return code. On success (when result == SSL_TLSEXT_ERR_OK),
 *      the OCSP_RESPONSE will be stored in the pointer passed-in as last
 *      argument.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
OCSP_computeResponse(SSL *ssl, const SSLCertStatusArg *srctx, OCSP_RESPONSE **resp)
{
    X509           *cert;
    OCSP_CERTID    *id;
    OCSP_REQUEST   *req = NULL;
    int             rc, result = SSL_TLSEXT_ERR_NOACK;
    bool            selfSigned = NS_FALSE;
    Tcl_DString     cachedResponseFile;
    STACK_OF(OPENSSL_STRING) *aia = NULL; /* Authority Information Access (AIA) Extension */

    NS_NONNULL_ASSERT(ssl != NULL);
    NS_NONNULL_ASSERT(srctx != NULL);
    NS_NONNULL_ASSERT(resp != NULL);

    Tcl_DStringInit(&cachedResponseFile);

    cert = SSL_get_certificate(ssl);
    id = OCSP_get_cert_id(ssl, cert, &selfSigned);
    if (id == NULL || selfSigned) {
        goto err;
    }

    aia = X509_get1_ocsp(cert);
    if (aia == NULL) {
        Ns_Log(Warning, "cert_status: cannot obtain URL for Authority Information Access (AIA), maybe self-signed?");
        goto err;
    }

    /*
     * Try to get the OCSP_RESPONSE from a cache file.
     */
    rc = OCSP_FromCacheFile(&cachedResponseFile, id, resp);
    if (rc == TCL_OK) {
        result = SSL_TLSEXT_ERR_OK;
        goto done;
    }

    req = OCSP_REQUEST_new();
    if (req == NULL || !OCSP_request_add0_id(req, id)) {
        goto err;

    } else {
        STACK_OF(X509_EXTENSION) *exts;
        int i;

        if (srctx->verbose) {
            Ns_Log(Notice, "cert_status: Authority Information Access (AIA) URL: %s",
                   sk_OPENSSL_STRING_value(aia, 0));
        }
        id = NULL;

        /*
         * Add extensions to the request.
         */
        SSL_get_tlsext_status_exts(ssl, &exts);
        for (i = 0; i < sk_X509_EXTENSION_num(exts); i++) {
            X509_EXTENSION *ext = sk_X509_EXTENSION_value(exts, i);

            if (!OCSP_REQUEST_add_ext(req, ext, -1)) {
                sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
                goto err;
            }
        }
        *resp = OCSP_FromAIA(req, sk_OPENSSL_STRING_value(aia, 0),
                             srctx->timeout);
        if (*resp == NULL) {
            Ns_Log(Warning, "cert_status: no OCSP response obtained");
            //result = SSL_TLSEXT_ERR_OK;
        } else {
            BIO        *derbio;
            const char *fileName = cachedResponseFile.string;

            /*
             * We have a valid OCSP response, save it in a disk cache file.
             */

            derbio = BIO_new_file(fileName, "wb");
            if (derbio == NULL) {
                Ns_Log(Warning, "cert_status: Cannot write to OCSP response file: %s", fileName);
            } else {
                i2d_OCSP_RESPONSE_bio(derbio, *resp);
                BIO_free(derbio);
            }

            result = SSL_TLSEXT_ERR_OK;
        }
        sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
        goto done;
    }

 err:
    result = SSL_TLSEXT_ERR_ALERT_FATAL;
 done:
    /*
     * If we parsed AIA we need to free
     */
    if (aia != NULL) {
        sk_OPENSSL_STRING_pop_free(aia, openssl_string_free);
    }
    OCSP_CERTID_free(id);
    OCSP_REQUEST_free(req);
    Tcl_DStringFree(&cachedResponseFile);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * OCSP_FromAIA --
 *
 *      Get OCSP_RESPONSE from the certificate issuing server (Authority
 *      Information Access) via the DER encoded OCSP request.
 *
 * Results:
 *      OCSP_RESPONSE * or NULL in case of failure.
 *
 * Side effects:
 *      Issue HTTP/HTTPS request to the AIA server.
 *
 *----------------------------------------------------------------------
 */

static OCSP_RESPONSE *
OCSP_FromAIA(OCSP_REQUEST *req, const char *aiaURL, int req_timeout)
{
    OCSP_RESPONSE *rsp = NULL;
    int            derLength;

    NS_NONNULL_ASSERT(req != NULL);

    if (aiaURL == NULL) {
        Ns_Log(Warning, "The certificate says it supports OCSP, but has a NULL AIA URL");
        return rsp;
    }
    Ns_Log(Notice, "OCSP_FromAIA url <%s> timeout %d", aiaURL, req_timeout);

    /*
     * We have already the OCSP request int *req with the ID to be queried
     * filled in.
     */

    derLength = i2d_OCSP_REQUEST(req, NULL);
    if (derLength <= 0) {
        Ns_Log(Error, "cert_status: invalid OCSP request size");

    } else {
        Tcl_DString    dsBinary, dsBase64, dsCMD;
        unsigned char *ppout;
        TCL_SIZE_T     base64len;

        Tcl_DStringInit(&dsBinary);
        Tcl_DStringInit(&dsBase64);
        Tcl_DStringInit(&dsCMD);

        Tcl_DStringSetLength(&dsBinary, (TCL_SIZE_T)derLength + 1);
        ppout = (unsigned char *)dsBinary.string;
        derLength = i2d_OCSP_REQUEST(req, &ppout);

        /*
         * Append DER encoding of the OCSP request via URL-encoding of base64
         * encoding, as defined in https://tools.ietf.org/html/rfc6960#appendix-A
         */
        base64len = MAX(4, ((TCL_SIZE_T)derLength * 4/3) + 4);
        Tcl_DStringSetLength(&dsBase64, base64len);
        (void) Ns_Base64Encode((unsigned char *)dsBinary.string,
                               (size_t)derLength, dsBase64.string,
                               0, 0);
        Tcl_DStringAppend(&dsCMD, "ns_http run ", TCL_INDEX_NONE);
        Tcl_DStringAppend(&dsCMD, aiaURL, TCL_INDEX_NONE);

        /*
         * Append slash to URI if necessary.
         */
        if (dsCMD.string[dsCMD.length-1] != '/') {
            Tcl_DStringAppend(&dsCMD, "/", 1);
        }
        /*
         * Append URL encoded base64 encoding of OCSP.
         */
        Ns_UrlPathEncode(&dsCMD, dsBase64.string, NULL);

        {
            // maybe we can get an interpreter from the SSLContext, depending of being
            // able to pass Ns_Sock to callback, or to access it earlier via a push
            // into into the ocsp context
            Tcl_Interp *interp = Ns_TclAllocateInterp(nsconf.defaultServer);

            if (interp != NULL) {
                Tcl_Obj    *resultObj;
                Tcl_DString dsResult;

                Tcl_DStringInit(&dsResult);
                Ns_Log(Notice, "OCSP command: %s\n", dsCMD.string);

                if (Tcl_EvalEx(interp, dsCMD.string, dsCMD.length, 0) != TCL_OK) {
                    resultObj = Tcl_GetObjResult(interp);
                    Ns_Log(Error, "OCSP_REQUEST '%s' returned error '%s'", dsCMD.string, Tcl_GetString(resultObj));
                } else {
                    Tcl_Obj *statusObj = Tcl_NewStringObj("status", TCL_INDEX_NONE);
                    Tcl_Obj *bodyObj = Tcl_NewStringObj("body", TCL_INDEX_NONE);
                    Tcl_Obj *valueObj = NULL;
                    Ns_ReturnCode status;

                    resultObj = Tcl_GetObjResult(interp);
                    Tcl_IncrRefCount(resultObj);

                    if (Tcl_DictObjGet(interp, resultObj, statusObj, &valueObj) == TCL_OK
                        && valueObj != NULL
                        ) {
                        const char *stringValue =  Tcl_GetString(valueObj);
                        /*
                         * Check, if the HTTP status code starts with a '2'.
                         */
                        if (*stringValue == '2') {
                            status = NS_OK;
                        } else {
                            Ns_Log(Warning, "OCSP request returns status code %s from AIA %s",
                                   stringValue, dsCMD.string);
                            status = NS_ERROR;
                        }
                    } else {
                        Ns_Log(Warning, "OCSP_REQUEST: dict has no 'status' %s",
                               Tcl_GetString(resultObj));
                        status = NS_ERROR;
                    }
                    if (status == NS_OK) {
                        if (Tcl_DictObjGet(interp, resultObj, bodyObj, &valueObj) == TCL_OK) {
                            TCL_SIZE_T           length;
                            const unsigned char *bytes;

                            bytes = Tcl_GetByteArrayFromObj(valueObj, &length);
                            rsp = d2i_OCSP_RESPONSE(NULL, &bytes, length);
                        }
                    }
                    Tcl_DecrRefCount(resultObj);
                    Tcl_DecrRefCount(statusObj);
                    Tcl_DecrRefCount(bodyObj);

                }
                Ns_TclDeAllocateInterp(interp);
                Tcl_DStringFree(&dsResult);
            }
        }

        Tcl_DStringFree(&dsBinary);
        Tcl_DStringFree(&dsBase64);
        Tcl_DStringFree(&dsCMD);
    }

    return rsp;
}

#endif /* Of OPENSSL_NO_OCSP */

# if !defined(HAVE_OPENSSL_PRE_1_1) && !defined(LIBRESSL_VERSION_NUMBER)
static void *NS_CRYPTO_malloc(size_t num, const char *UNUSED(file), int UNUSED(line))
{
    return ns_malloc(num);
}
static void *NS_CRYPTO_realloc(void *addr, size_t num, const char *UNUSED(file), int UNUSED(line))
{
    return ns_realloc(addr, num);
}
static void NS_CRYPTO_free(void *addr, const char *UNUSED(file), int UNUSED(line))
{
    ns_free(addr);
}
# endif



/*
 *----------------------------------------------------------------------
 *
 * NsInitOpenSSL --
 *
 *      Library entry point for OpenSSL. This routine calls various
 *      initialization functions for OpenSSL. OpenSSL cannot be used
 *      before this function is called.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Numerous inside OpenSSL.
 *
 *----------------------------------------------------------------------
 */

void
NsInitOpenSSL(void)
{
# ifdef HAVE_OPENSSL_EVP_H
    static int initialized = 0;

    if (!initialized) {
        /*
         * With the release of OpenSSL 1.1.0 the interface of
         * CRYPTO_set_mem_functions() changed. Before that, we could
         * register the ns_* malloc functions without change, later
         * the types received two additional arguments for debugging
         * (line and line number). With the advent of OpenSSL 3.0,
         * function prototypes were introduced CRYPTO_malloc_fn,
         * CRYPTO_realloc_fn and CRYPTO_free_fn.
         */
#  if defined(HAVE_OPENSSL_PRE_1_1) || defined(LIBRESSL_VERSION_NUMBER)
        CRYPTO_set_mem_functions(ns_malloc, ns_realloc, ns_free);
#  else
        CRYPTO_set_mem_functions(NS_CRYPTO_malloc, NS_CRYPTO_realloc, NS_CRYPTO_free);
#  endif
        /*
         * With OpenSSL 1.1.0 or above the OpenSSL library initializes
         * itself automatically.
         */
#  if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_1_0_2)
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
#   if OPENSSL_VERSION_NUMBER < 0x010100000 || defined(LIBRESSL_1_0_2)
        SSL_library_init();
#   endif
#  else
        OPENSSL_init_ssl(0, NULL);
#  endif
        ClientCtxDataIndex = SSL_CTX_get_ex_new_index(0, (char*)"NaviServer Client Info", NULL, NULL, NULL);
        initialized = 1;
        /*
         * We do not want to get this message when, e.g., the nsproxy
         * helper is started.
         */
        if (nsconf.argv0 != NULL) {
            Ns_Log(Notice, "%s initialized (pid %d)",
                   SSLeay_version(SSLEAY_VERSION), getpid());
        }

        CertTableInit();
    }
# endif
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxClientCreate --
 *
 *      Create and Initialize OpenSSL context
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */


int
Ns_TLS_CtxClientCreate(Tcl_Interp *interp,
                       const char *cert, const char *caFile, const char *caPath, bool verify,
                       NS_TLS_SSL_CTX **ctxPtr)
{
    NS_TLS_SSL_CTX *ctx;
    char errorBuffer[256];

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctxPtr != NULL);

    ctx = SSL_CTX_new(SSLv23_client_method());
    *ctxPtr = ctx;
    if (ctx == NULL) {
        Ns_TclPrintfResult(interp, "ctx init failed: %s", ERR_error_string(ERR_get_error(), errorBuffer));
        return TCL_ERROR;
    }

    SSL_CTX_set_default_verify_paths(ctx);
    if (verify && (caFile != NULL || caPath != NULL)) {
        int rc;

        rc = SSL_CTX_load_verify_locations(ctx, caFile, caPath);
        if (rc == 0) {
            Ns_TclPrintfResult(interp, "cannot load cerfificates from CAfile %s and CApath %s: %s",
                               caFile != NULL ? caFile : "none",
                               caPath != NULL ? caPath : "none",
                               ERR_error_string(ERR_get_error(), errorBuffer));
            goto fail;
        }
        Ns_Log(Debug, "SSL_CTX_load_verify_locations ctx %p caFile <%s> caPath <%s> ",
               (void*)ctx,
               caFile != NULL ? caFile : "none",
               caPath != NULL ? caPath : "none");
    }
    if (verify) {
        int verify_depth = 9;
        NsInterp *itPtr = NsGetInterpData(interp);
        SSL_verify_cb verifyCB = NULL;

        if (itPtr != NULL) {
            NsServer *servPtr = itPtr->servPtr;

            if (likely(servPtr != NULL)) {
                /*
                 * We can set the specified validation depth. In case
                 * we have validation exception provided, we register
                 * our own certificate validation callback.
                 */
                verify_depth = servPtr->httpclient.verify_depth;
                SSL_CTX_set_ex_data(ctx, ClientCtxDataIndex, servPtr);

                if (servPtr->httpclient.validationExceptions.size > 0) {
                    Ns_Log(Debug, "Ns_TLS_CtxClientCreate %ld validation exceptions provided",
                           servPtr->httpclient.validationExceptions.size);
                    verifyCB = CertficateValidationCB;
                }
            } else {
                Ns_Log(Warning, "Ns_TLS_CtxClientCreate cannot obtain server information;"
                       " detailed validation settings are ignored");
                SSL_CTX_set_ex_data(ctx, ClientCtxDataIndex, NULL);
            }
        }
        SSL_CTX_set_verify_depth(ctx, verify_depth + 1);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verifyCB);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    //SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (cert != NULL) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            Ns_TclPrintfResult(interp, "certificate load error: %s",
                               ERR_error_string(ERR_get_error(), errorBuffer));
            goto fail;
        }

        if (SSL_CTX_use_PrivateKey_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
            Ns_TclPrintfResult(interp, "private key load error: %s", ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
    }

    return TCL_OK;

 fail:
    SSL_CTX_free(ctx);
    *ctxPtr = NULL;

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxFree --
 *
 *      Free OpenSSL context
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *ctx)
{
    NS_NONNULL_ASSERT(ctx != NULL);

    SSL_CTX_free(ctx);
}

/*
 *----------------------------------------------------------------------
 *
 * WaitFor --
 *
 *      Wait for the provided or default 10ms (currently hardcoded)
 *      for a state change on a socket.  This is used for handling
 *      OpenSSL's states SSL_ERROR_WANT_READ and SSL_ERROR_WANT_WRITE.
 *
 * Results:
 *      NS_OK, NS_ERROR, or NS_TIMEOUT.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
WaitFor(NS_SOCKET sock, unsigned int st, const Ns_Time *timeoutPtr)
{
    Ns_Time timeout;
    if (timeoutPtr == NULL) {
        /* 10ms */
        timeout.sec = 0;
        timeout.usec = 10000;
        timeoutPtr = &timeout;
    }
    return Ns_SockTimedWait(sock, st, timeoutPtr);
}

static Ns_ReturnCode
PartialTimeout(const Ns_Time *endTimePtr, Ns_Time *diffPtr, Ns_Time *defaultPartialTimeoutPtr,
               Ns_Time **partialTimeoutPtrPtr)
{
    Ns_Time       now;
    Ns_ReturnCode result = NS_OK;

    Ns_GetTime(&now);
    if (Ns_DiffTime(endTimePtr, &now, diffPtr) > 0) {
        if (diffPtr->sec > defaultPartialTimeoutPtr->sec || diffPtr->usec > defaultPartialTimeoutPtr->usec) {
            *partialTimeoutPtrPtr = defaultPartialTimeoutPtr;
        } else {
            *partialTimeoutPtrPtr = diffPtr;
        }
        Ns_Log(Debug, "Ns_TLS_SSLConnect partial timeout " NS_TIME_FMT,
               (int64_t)(*partialTimeoutPtrPtr)->sec, (*partialTimeoutPtrPtr)->usec);
    } else {
        /* time is up */
        result = NS_TIMEOUT;
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_SSLConnect --
 *
 *      Initialize a socket as ssl socket and wait until the socket is
 *      usable (is connected, handshake performed)
 *
 * Results:
 *      NS_OK, NS_ERROR, or NS_TIMEOUT.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_TLS_SSLConnect(Tcl_Interp *interp, NS_SOCKET sock, NS_TLS_SSL_CTX *ctx,
                  const char *sni_hostname, const char *caFile, const char *caPath,
                  const Ns_Time *timeoutPtr, NS_TLS_SSL **sslPtr)
{
    NS_TLS_SSL     *ssl;
    Ns_ReturnCode   result = NS_OK;
    Ns_Time         endTime, defaultPartialTimeout = { 0, 10000 }; /* 10ms */

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(sslPtr != NULL);

    if (timeoutPtr != NULL) {
        Ns_GetTime(&endTime);
        Ns_IncrTime(&endTime, timeoutPtr->sec, timeoutPtr->usec);
    }

    ssl = SSL_new(ctx);
    *sslPtr = ssl;
    if (ssl == NULL) {
        Ns_TclPrintfResult(interp, "SSLCreate failed: %s", ERR_error_string(ERR_get_error(), NULL));
        result = NS_ERROR;

    } else {
        int sslErr;

        if (sni_hostname != NULL) {
# if HAVE_SSL_set_tlsext_host_name
            Ns_Log(Debug, "tls: setting SNI hostname '%s'", sni_hostname);
            if (SSL_set_tlsext_host_name(ssl, sni_hostname) != 1) {
                Ns_Log(Warning, "tls: setting SNI hostname '%s' failed, value ignored", sni_hostname);
            }
# else
            Ns_Log(Warning, "tls: SNI hostname '%s' is not supported by version of OpenSSL", sni_hostname);
# endif
        }
        SSL_set_fd(ssl, sock);
        SSL_set_connect_state(ssl);

        for (;;) {
            int           sslRc;
            Ns_Time       timeout, *partialTimeoutPtr = NULL;

            Ns_Log(Debug, "ssl connect on sock %d", sock);
            sslRc  = SSL_connect(ssl);
            sslErr = SSL_get_error(ssl, sslRc);

            //fprintf(stderr, "### ssl connect sock %d returned err %d\n", sock, sslErr);

            if (sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE) {
                if (timeoutPtr != NULL) {
                    /*
                     * Since there might be many WANT_READ or
                     * WANT_WRITE, we have to constantly check the
                     * remaining time and may adjust the partial
                     * timeout as well.
                     */
                    if (PartialTimeout(&endTime, &timeout, &defaultPartialTimeout, &partialTimeoutPtr) == NS_TIMEOUT) {
                        result = NS_TIMEOUT;
                        break;
                    }
                }
                (void) WaitFor(sock,
                               (unsigned int)(sslErr == SSL_ERROR_WANT_READ ? NS_SOCK_READ : NS_SOCK_WRITE),
                               partialTimeoutPtr);
                continue;
            }
            break;
        }

        /*Ns_Log(Notice, "============================= CONNECT result %s finished %d ERROR %d (is SSL_ERROR_SSL %d)",
               Ns_ReturnCodeString(result), SSL_is_init_finished(ssl),
               sslErr, sslErr == SSL_ERROR_SSL);*/

        if (result == NS_OK && !SSL_is_init_finished(ssl)) {
            long  x509err = X509_V_OK;

            Ns_Log(Debug, "CONNECT ERROR %d (is SSL_ERROR_SSL %d)",  sslErr, sslErr == SSL_ERROR_SSL);

            if (sslErr == SSL_ERROR_SSL) {
                Ns_Log(Debug, "CONNECT SSL_ERROR_SSL: A failure in the SSL library occurred: %s", ERR_error_string(ERR_get_error(), NULL));
                x509err = SSL_get_verify_result(ssl);
                if (x509err != X509_V_OK) {
                    /*
                     * We have a specific error code for the certificate validation failure.
                     */
                    Ns_TclPrintfResult(interp, "ssl connect failed: %s (reason %ld: %s)",
                                       ERR_error_string(ERR_get_error(), NULL),
                                       x509err,  X509_verify_cert_error_string(x509err));
                    Ns_Log(Notice, "certificate validation error: %s\nCAfile: %s\nCApath: %s",
                           X509_verify_cert_error_string(x509err),
                           caFile, caPath);
                    //X509_NAME_oneline(X509_get_issuer_name(err_cert), errorbuf, sizeof(errorbuf));
                }
            }
            if (x509err == X509_V_OK) {
                Ns_TclPrintfResult(interp, "ssl connect failed: %s", ERR_error_string(ERR_get_error(), NULL));
            }
            DrainErrorStack(Warning, "ssl connect", ERR_get_error());

            result = NS_ERROR;
        } else {
            //const char *verifyString = X509_verify_cert_error_string(SSL_get_verify_result(ssl));
            //fprintf(stderr, "### SSL certificate verify: %s\n", verifyString);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * FilenameToEnvVar --
 *
 *      Convert a file path into a valid environment variable name by
 *      prefixing it with "TLS_KEY_PASS_" and replacing non-alphanumeric
 *      characters with underscores, while uppercasing letters.
 *
 * Parameters:
 *      dsPtr    - pointer to an initialized Tcl_DString to append to
 *      filename - NUL-terminated file path string to convert
 *
 * Returns:
 *      A pointer to the Tcl_DString's internal buffer (dsPtr->string),
 *      which now contains the generated environment variable name.
 *
 * Side Effects:
 *      Appends "TLS_KEY_PASS_" and then the mapped characters of 'filename'
 *      to the Tcl_DString.  Caller is responsible for freeing or resetting
 *      the Tcl_DString when done.
 *
 *----------------------------------------------------------------------
 */
static char *
FilenameToEnvVar(Tcl_DString *dsPtr, const char *filename) {
    size_t inputLength, i;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(filename != NULL);

    inputLength = strlen(filename);
    Tcl_DStringAppend(dsPtr, "TLS_KEY_PASS_", 13);

    for (i = 0; i < inputLength; ++i) {
        unsigned char c = (unsigned char) filename[i];
        if (CHARTYPE(alnum, c) != 0) {
            c = (unsigned char)toupper(c);
        } else {
            c = '_';
        }
        Tcl_DStringAppend(dsPtr, (const char*)&c, 1);
    }

    return dsPtr->string;
}

/*
 * Helper: ExecuteKeyScript --
 *
 *    If a "tlskeyproc" helper is configured, resolve its filename relative
 *    to nsconf.binDir, build a Tcl exec command, and run it to obtain
 *    a private-key passphrase for `pem_path`.
 *
 * Parameters:
 *    dsPtr      - initialized Tcl_DString used for command construction.
 *    section    - configuration section name (for path resolution).
 *    pem_path   - filesystem path to the encrypted PEM file.
 *    out_pass   - output buffer; on return, contains the passphrase
 *                 (must be freed by caller via Tcl_DStringFree).
 *
 * Returns:
 *    NS_OK      if the helper returned a non-empty passphrase;
 *    NS_ERROR   on evaluation failure or empty output.
 *
 * Side Effects:
 *    May call Ns_ConfigFilename(), Ns_ConfigGetValue(), and Ns_TclEval().
 */
static Ns_ReturnCode
ExecuteKeyScript(Tcl_DString *dsPtr, const char *scriptPath, const char *pemPath)
{
    Ns_ReturnCode rc;
    Tcl_DString   resultDs;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(scriptPath != NULL);

    Tcl_DStringAppend(dsPtr, "exec -ignorestderr", 18);
    Tcl_DStringAppendElement(dsPtr, scriptPath);
    if (pemPath != NULL) {
        Tcl_DStringAppendElement(dsPtr, pemPath);
    }

    Tcl_DStringInit(&resultDs);
    rc = Ns_TclEval(&resultDs, NULL, dsPtr->string);
    /*
     * Reset input dsPtr to prepare for storing the result.
     */
    Tcl_DStringSetLength(dsPtr, 0);

    if (rc != NS_OK || resultDs.length == 0) {
        Tcl_DStringFree(&resultDs);
        rc = NS_ERROR;
    } else {
        /*
         * Copy helper output into dsPtr for caller to use
         */
        Tcl_DStringAppend(dsPtr, resultDs.string, resultDs.length);
        Tcl_DStringFree(&resultDs);
        rc = NS_OK;
    }
    return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * TLSPasswordCB --
 *
 *      OpenSSL callback to obtain the passphrase for an encrypted
 *      private key.  The lookup proceeds in three stages:
 *
 *        1) If the server’s NsSSLConfig has a tlsKeyScript defined,
 *           run that helper script (`ExecuteKeyScript`) with the PEM
 *           filename.  If it returns NS_OK, its output (in ds.string)
 *           is used as the passphrase.
 *
 *        2) Otherwise, derive an environment‐variable name from the
 *           PEM path (via FilenameToEnvVar) and look up that variable.
 *           If not set, fall back to the generic TLS_KEY_PASS variable.
 *
 *        3) As a last resort, prompt the user on stdin with
 *           “Enter TLS password:” and read from the console.
 *
 * Parameters:
 *      buf    – buffer in which to store the passphrase
 *      size   – size of buf (including space for terminating NUL)
 *      rwflag – read/write flag (unused in this implementation)
 *      userdata – pointer to the NUL-terminated PEM file path string
 *
 * Returns:
 *      The number of bytes copied into buf (excluding the NUL), or 0
 *      if no passphrase could be obtained (e.g. pemPath==NULL or EOF).
 *
 * Side Effects:
 *      May invoke a Tcl script via ExecuteKeyScript(), read environment
 *      variables, allocate/append to a Tcl_DString, and block on stdin
 *      if prompting is necessary.
 *
 *----------------------------------------------------------------------
 */
static int
TLSPasswordCB(char *buf, int size, int UNUSED(rwflag), void *userdata)
{
    size_t        len = 0;
    Tcl_DString   ds;
    const char   *pwd = NULL;
    const char   *pemPath = (const char *)userdata;

    if (pemPath == NULL) {
        return 0;
    }

    Ns_Log(Debug, "TLSPasswordCB");
    Tcl_DStringInit(&ds);

    {
        NS_TLS_SSL_CTX *ctxPtr = CertTableGetCtx(pemPath);
        NsSSLConfig    *cfgPtr = SSL_CTX_get_app_data(ctxPtr);

        /*
         * 1) Try to get secret from external helper script
         */
        if (cfgPtr  != NULL && cfgPtr->tlsKeyScript != NULL) {
            Ns_Log(Notice, "TLS key from script <%s>", cfgPtr->tlsKeyScript);
            if (ExecuteKeyScript(&ds, cfgPtr->tlsKeyScript, pemPath) == NS_OK) {
                pwd = ds.string;
            }
        }
    }

    /*
     * 2) Environment variables
     */
    if (pwd == NULL) {
        const char *env = FilenameToEnvVar(&ds, pemPath);
        Ns_Log(Notice, "TLS key from environment <%s>", env);
        pwd = getenv(env);
    }
    if (pwd == NULL) {
        Ns_Log(Notice, "TLS key from environment <TLS_KEY_PASS>");
        pwd = getenv("TLS_KEY_PASS");
    }

    /*
     * 3) Prompt if still not found
     */
    if (pwd == NULL) {
        goto prompt;
    }

    assert(pwd != NULL);

    len = strlen(pwd);
    if (len >= (size_t) size) {
        len = (size_t)size - 1;
    }

    memcpy(buf, pwd, len);
    buf[len] = '\0';
    Tcl_DStringFree(&ds);

    return (int) len;

 prompt:
    Tcl_DStringFree(&ds);

    fprintf(stdout, "Enter TLS password:");
    if (fgets(buf, size, stdin) != NULL) {
        len = strlen(buf);
    }
    return (int)len;
}

/*
 *----------------------------------------------------------------------
 *
 * DrainErrorStack --
 *
 *      Report 0 to n errors from the OpenSSL error stack. This
 *      function reports the errors and clears it as well.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Error reporting.
 *
 *----------------------------------------------------------------------
 */
static void
DrainErrorStack(Ns_LogSeverity severity, const char *errorContext, unsigned long sslERRcode)
{
    char errorBuffer[256];

    while (sslERRcode != 0u) {
        Ns_Log(severity, "%s: OpenSSL errorCode:%lu errorString: %s",
               errorContext, sslERRcode, ERR_error_string(sslERRcode, errorBuffer));

        sslERRcode = ERR_get_error();
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsSSLConfigNew --
 *
 *      Creates a new NsSSLConfig structure and sets standard
 *      configuration parameters ("deferaccept", "nodelay", and "verify").
 *
 * Results:
 *      Pointer to a new NsSSLConfig.
 *
 * Side effects:
 *      Allocating memory.
 *
 *----------------------------------------------------------------------
 */
NsSSLConfig *
NsSSLConfigNew(const char *section)
{
    NsSSLConfig *drvCfgPtr;

    drvCfgPtr = ns_calloc(1, sizeof(NsSSLConfig));
    drvCfgPtr->deferaccept   = Ns_ConfigBool(section, "deferaccept", NS_FALSE);
    drvCfgPtr->nodelay       = Ns_ConfigBool(section, "nodelay", NS_TRUE);
    drvCfgPtr->verify        = Ns_ConfigBool(section, "verify", 0);
    drvCfgPtr->tlsKeylogFile = Ns_ConfigGetValue(section, "tlskeylogfile");
    drvCfgPtr->tlsKeyScript  = Ns_ConfigGetValue(section, "tlskeyscript");
    if (drvCfgPtr->tlsKeyScript != NULL) {
        drvCfgPtr->tlsKeyScript = Ns_ConfigFilename(section, "tlskeyscript", 12,
                                                    nsconf.binDir, /* directory to resolve against */
                                                    ""             /* default no script */,
                                                    NS_TRUE /*normalize*/, NS_FALSE /*updateCfg*/);
    }
    /*
     * In case "vhostcertificates" was specified in the configuration file,
     * and it is valid, activate NS_DRIVER_SNI.
     */
    drvCfgPtr->vhostcertificates = Ns_ConfigGetValue(section, "vhostcertificates");
    if (drvCfgPtr->vhostcertificates != NULL && *drvCfgPtr->vhostcertificates != '\0') {
        struct stat st;

        if (stat(drvCfgPtr->vhostcertificates, &st) != 0) {
            Ns_Log(Warning, "vhostcertificates directory '%s' does not exist",
                   drvCfgPtr->vhostcertificates);
            drvCfgPtr->vhostcertificates = NULL;
        } else if (S_ISDIR(st.st_mode) == 0) {
            Ns_Log(Warning, "value specified for vhostcertificates is not a directory: '%s'",
                   drvCfgPtr->vhostcertificates);
            drvCfgPtr->vhostcertificates = NULL;
        } else {
            Ns_Log(Notice, "vhostcertificates directory '%s' is valid, activating SNI",
                   drvCfgPtr->vhostcertificates);
        }
    }
    return drvCfgPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * BuildALPNWireFormat --
 *
 *      Encode a comma-separated list of ALPN protocol identifiers into
 *      the TLS ALPN wire format and store the result in a Tcl_DString.
 *      Each protocol name is prefixed by its length (one byte), as required
 *      by SSL_CTX_set_alpn_protos or SSL_set_alpn_protos.
 *
 * Parameters:
 *      dsPtr   - pointer to an initialized Tcl_DString; on return its
 *                buffer (dsPtr->string) contains the encoded wire format.
 *      alpnStr - NUL-terminated, comma-separated list of protocol names
 *                (e.g. "h2,http/1.1"); must not be NULL.
 *
 * Returns:
 *      A pointer to the internal string buffer of dsPtr (cast to unsigned char *).
 *      If any protocol name is empty or longer than 255 bytes, returns NS_ERROR
 *      (and leaves dsPtr unmodified). Otherwise NS_OK is returned.
 *
 * Side Effects:
 *      Repeatedly resizes dsPtr to accommodate the growing wire format,
 *      and writes length-prefixed protocol names into its buffer.
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
BuildALPNWireFormat(Tcl_DString *dsPtr, const char *alpnStr)
{
    const char    *start = alpnStr;
    const char    *end;
    size_t         len = 0;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(alpnStr != NULL);

    while (*start != '\0') {
        size_t plen;

        end = strchr(start, ',');
        if (end == NULL) {
            end = start + strlen(start);
        }
        plen = (size_t)(end - start);

        if (plen == 0 || plen > 255) {
            return NS_ERROR;
        }

        Tcl_DStringSetLength(dsPtr, (TCL_SIZE_T)(len + 1 + plen));
        dsPtr->string[len++] = (char)plen;
        memcpy(&dsPtr->string[len], start, plen);
        len += plen;

        if (*end == '\0') {
            break;
        }
        start = end + 1;
    }
    Tcl_DStringSetLength(dsPtr, (int)len);

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ALPNSelectCB --
 *
 *      OpenSSL ALPN (Application-Layer Protocol Negotiation) selection
 *      callback.  When a TLS client offers a list of supported protocols,
 *      this function picks the first one that the server also supports,
 *      using OpenSSL’s built‐in SSL_select_next_proto() helper.
 *
 *      The server’s protocol list is passed in via the "arg" pointer,
 *      but we pack the length immediately before the data in memory,
 *      so we reconstruct it by reading the unsigned int stored just
 *      before "arg".
 *
 * Parameters:
 *      ssl                   - The current SSL connection (unused).
 *      out                   - On success, set to point to the chosen
 *                              protocol bytes within serverProtos.
 *      outlen                - On success, set to the length of the
 *                              chosen protocol.
 *      clientProtos          - Wire‐format list of protocols offered by
 *                              the client (each prefixed by its length).
 *      clientProtosLength    - Total byte length of clientProtos.
 *      arg                   - Pointer to the server’s wire‐format
 *                              protocols list; its length is stored
 *                              as an unsigned int immediately before
 *                              the pointer in memory.
 *
 * Returns:
 *      SSL_TLSEXT_ERR_OK     if a common protocol was negotiated
 *      SSL_TLSEXT_ERR_NOACK  otherwise (no overlap or negotiation failure)
 *
 * Side Effects:
 *      Logs the return code from SSL_select_next_proto() at DEBUG level.
 *
 *----------------------------------------------------------------------
 */
static int
ALPNSelectCB(NS_TLS_SSL *UNUSED(ssl), const unsigned char **out, unsigned char *outlen,
             const unsigned char *clientProtos, unsigned int clientProtosLength, void *arg)
{
    const unsigned char *serverProtos = arg;
    unsigned int serverProtosLength = *(unsigned int *)((char *)arg - sizeof(unsigned int));

    int rc = SSL_select_next_proto((unsigned char **)out, outlen,
                                   serverProtos, serverProtosLength,
                                   clientProtos, clientProtosLength);

    Ns_Log(Debug, "ALPNSelectCB returns %d", rc);
    return (rc == OPENSSL_NPN_NEGOTIATED) ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_NOACK;
}

# if OPENSSL_VERSION_NUMBER >= 0x10101000L
static void KeylogCB(const SSL *ssl, const char *line)
{
    NsSSLConfig *cfgPtr;

    NS_NONNULL_ASSERT(ssl != NULL);

    cfgPtr = (NsSSLConfig *)SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    assert(cfgPtr != NULL);

    /*Ns_Log(Notice, "KeylogCB called with: %s", line);*/
    if (!keylog_fp) {
        const char *path;
        if (cfgPtr->tlsKeylogFile != NULL && *cfgPtr->tlsKeylogFile != '\0') {
            path = cfgPtr->tlsKeylogFile;
        } else {
            path = getenv("SSLKEYLOGFILE");
            if (!path) {
                path = "/tmp/sslkeylog.log";   /* fallback */
            }
        }
        keylog_fp = fopen(path, "a");
        if (!keylog_fp) {
            perror("fopen(keylog)");
            return;
        }
    }

    fprintf(keylog_fp, "%s\n", line);
    fflush(keylog_fp);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxServerInit --
 *
 *      Read config information, create and initialize OpenSSL
 *      context.  This function is called at startup to define OpenSSL
 *      contexts for the defined servers.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Ns_TLS_CtxServerInit(const char *section, Tcl_Interp *interp,
                     unsigned int flags,
                     void *app_data,
                     NS_TLS_SSL_CTX **ctxPtr)
{
    int         result;
    const char *cert;

    cert = Ns_ConfigGetValue(section, "certificate");
    Ns_Log(Notice, "load certificate '%s' specified in section %s", cert, section);

    if (cert == NULL) {
        Ns_Log(Error, "nsssl: certificate parameter must be specified in the configuration file under %s", section);
        result = TCL_ERROR;
    } else {
        const char *ciphers, *ciphersuites, *protocols;
        Ns_DList dl, *dlPtr = &dl;

        /*
         * Keep configuration values in an Ns_DList to protect against
         * potential changes in the configuration Ns_Set.
         */
        Ns_DListInit(dlPtr);

        cert         = Ns_DListSaveString(dlPtr, cert);
        ciphers      = Ns_DListSaveString(dlPtr, Ns_ConfigGetValue(section, "ciphers"));
        ciphersuites = Ns_DListSaveString(dlPtr, Ns_ConfigGetValue(section, "ciphersuites"));
        protocols    = Ns_DListSaveString(dlPtr, Ns_ConfigGetValue(section, "protocols"));

        Ns_Log(Debug, "Ns_TLS_CtxServerInit calls Ns_TLS_CtxServerCreate with app data %p",
               (void*) app_data);

        result = Ns_TLS_CtxServerCreateCfg(interp, cert,
                                           NULL /*caFile*/, NULL /*caPath*/,
                                           Ns_ConfigBool(section, "verify", 0),
                                           ciphers, ciphersuites, protocols,
                                           "http/1.1",
                                           app_data,
                                           ctxPtr);
        if (result == TCL_OK) {
            NsSSLConfig *cfgPtr = app_data;

            if (cfgPtr == NULL) {
                /*
                 * Get the app_data (cfgPtr) from the SSL_CTX.
                 */
                cfgPtr = SSL_CTX_get_app_data(*ctxPtr);

            }
            if (cfgPtr == NULL) {
                /*
                 * Create new app_data (= NsSSLConfig).
                 *
                 * The app_data of SSL_CTX is cfgPtr (NsSSLConfig*),
                 * while the app_data of an SSL connection is the
                 * sockPtr (Ns_Sock*).
                 */
                cfgPtr = NsSSLConfigNew(section);
                cfgPtr->ctx = *ctxPtr;
                Ns_Log(Debug, "Ns_TLS_CtxServerInit created new app data %p for cert <%s> ctx %p",
                        (void*)cfgPtr, cert, (void*)(cfgPtr->ctx));
                SSL_CTX_set_app_data(*ctxPtr, (void *)cfgPtr);
            }

            SSL_CTX_set_session_id_context(*ctxPtr, (const unsigned char *)&nsconf.pid, sizeof(pid_t));
            SSL_CTX_set_session_cache_mode(*ctxPtr, SSL_SESS_CACHE_SERVER);

#ifdef HAVE_OPENSSL_PRE_1_1
            SSL_CTX_set_info_callback(*ctxPtr, SSL_infoCB);
#endif

            SSL_CTX_set_options(*ctxPtr, SSL_OP_NO_SSLv2);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_SINGLE_DH_USE);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

            /*
             * Prefer server ciphers to secure against BEAST attack.
             */
            SSL_CTX_set_options(*ctxPtr, SSL_OP_CIPHER_SERVER_PREFERENCE);
            /*
             * Disable compression to avoid CRIME attack.
             */
#ifdef SSL_OP_NO_COMPRESSION
            SSL_CTX_set_options(*ctxPtr, SSL_OP_NO_COMPRESSION);
#endif
            /*
             * Since EOF behavior of OpenSSL concerning EOF handling
             * changed, we could consider using
             *     SSL_OP_IGNORE_UNEXPECTED_EOF
             *
             * Other recent options to explore:
             *     SSL_MODE_ASYNC
             */

#ifdef OPENSSL_HAVE_READ_BUFFER_LEN
            /*
             * read_buffer_len is apparently just useful, when crypto
             * pipelining is set up. In general, the OpenSSL "dasync"
             * engine provides AES128-SHA based ciphers that have this
             * capability. However, these are so far for development
             * and test purposes only.
             *
             *  SSL_CTX_set_default_read_buffer_len(*ctxPtr, 65000);
             */
#endif

            /*
             * Obsolete since 1.1.0 but also supported in 3.0
             */
            SSL_CTX_set_options(*ctxPtr, SSL_OP_SSLEAY_080_CLIENT_DH_BUG);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_TLS_D5_BUG);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_TLS_BLOCK_PADDING_BUG);

            if ((flags & NS_DRIVER_SNI) != 0) {
                SSL_CTX_set_tlsext_servername_callback(*ctxPtr, SSL_serverNameCB);
                /* SSL_CTX_set_tlsext_servername_arg(cfgPtr->ctx, app_data); // not really needed */
            }
#ifdef OPENSSL_HAVE_DH_AUTO
            SSL_CTX_set_dh_auto(*ctxPtr, 1);
#else
            cfgPtr->dhKey512 = get_dh512();
            cfgPtr->dhKey1024 = get_dh1024();
            cfgPtr->dhKey2048 = get_dh2048();
            SSL_CTX_set_tmp_dh_callback(*ctxPtr, SSL_dhCB);
#endif

            {
                X509_STORE *storePtr;
                int rc;
                /*
                 * Initialize cert storage for the SSL_CTX; otherwise
                 * X509_STORE_CTX_get_* operations will fail.
                 */
                //#ifdef SSL_CTX_build_cert_chain
                //if (SSL_CTX_build_cert_chain(*ctxPtr, 0) != 1) {
                //    Ns_Log(Notice, "nsssl SSL_CTX_build_cert_chain failed");
                //}
                //#endif
                storePtr = SSL_CTX_get_cert_store(*ctxPtr /*SSL_get_SSL_CTX(s)*/);
                Ns_Log(Debug, "nsssl:SSL_CTX_get_cert_store %p", (void*)storePtr);

                rc = X509_STORE_load_locations(storePtr, cert, NULL);
                Ns_Log(Debug, "nsssl:X509_STORE_load_locations %d", rc);
            }
#ifndef OPENSSL_NO_OCSP
            if (Ns_ConfigBool(section, "ocspstapling", NS_FALSE)) {
                Ns_Log(Notice, "nsssl: activate OCSP stapling for %s", section);

                memset(&sslCertStatusArg, 0, sizeof(sslCertStatusArg));
                sslCertStatusArg.timeout = -1;
                sslCertStatusArg.verbose = Ns_ConfigBool(section, "ocspstaplingverbose", NS_FALSE);
                Ns_ConfigTimeUnitRange(section, "ocspcheckinterval",
                                       "5m", 1, 0, LONG_MAX, 0,
                                       &sslCertStatusArg.OCSPcheckInterval);

                SSL_CTX_set_tlsext_status_cb(*ctxPtr, SSL_cert_statusCB);
                SSL_CTX_set_tlsext_status_arg(*ctxPtr, &sslCertStatusArg);
            } else {
                Ns_Log(Notice, "nsssl: OCSP stapling for %s not activated", section);
            }
#endif

#if OPENSSL_VERSION_NUMBER > 0x00908070 && !defined(HAVE_OPENSSL_3) && !defined(OPENSSL_NO_EC)
            /*
             * Generate key for elliptic curve cryptography (potentially used
             * for Elliptic Curve Digital Signature Algorithm (ECDSA) and
             * Elliptic Curve Diffie-Hellman (ECDH).
             *
             * At least in OpenSSL3 secure re-negotiation is default.
             */
            {
                EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);

                if (ecdh == NULL) {
                    Ns_Log(Error, "nsssl: Couldn't obtain ecdh parameters");
                    return TCL_ERROR;
                }
                SSL_CTX_set_options(cfgPtr->ctx, SSL_OP_SINGLE_ECDH_USE);
                if (SSL_CTX_set_tmp_ecdh(cfgPtr->ctx, ecdh) != 1) {
                    Ns_Log(Error, "nsssl: Couldn't set ecdh parameters");
                    return TCL_ERROR;
                }
                EC_KEY_free (ecdh);
            }
#endif
        }
        Ns_DListFreeElements(dlPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CertTableInit, CertTableAdd, CertTableReload --
 *
 *      Static API for reloading certificates upon SIGHUP.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_HashTable certTable;
static void CertTableAdd(const NS_TLS_SSL_CTX *ctx, const char *cert)
{
    int            isNew = 0;
    Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(cert != NULL);

    Ns_MasterLock();
    hPtr = Tcl_CreateHashEntry(&certTable, (char *)ctx, &isNew);
    if (isNew != 0) {
        /*
         * Keep a local copy of the certificate string in case the
         * passed-in value is volatile.
         */
        Tcl_SetHashValue(hPtr, ns_strdup(cert));
        Ns_Log(Debug, "CertTableAdd: sslCtx %p cert '%s'", (void *)ctx, cert);
    }
    Ns_MasterUnlock();
}

static void CertTableInit(void)
{
    Tcl_InitHashTable(&certTable, TCL_ONE_WORD_KEYS);
    Ns_RegisterAtSignal((Ns_Callback *)(ns_funcptr_t)CertTableReload, NULL);
}

static void CertTableReload(void *UNUSED(arg))
{
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    Ns_MasterLock();
    hPtr = Tcl_FirstHashEntry(&certTable, &search);
    while (hPtr != NULL) {
        NS_TLS_SSL_CTX *ctx = (NS_TLS_SSL_CTX *)Tcl_GetHashKey(&certTable, hPtr);
        const char     *cert = Tcl_GetHashValue(hPtr);

        Ns_Log(Notice, "CertTableReload: sslCtx %p cert '%s'", (void *)ctx, cert);

        /*
         * Reload certificate and private key
         */
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            Ns_Log(Warning, "certificate reload error: %s", ERR_error_string(ERR_get_error(), NULL));

        } else if (SSL_CTX_use_PrivateKey_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
            Ns_Log(Warning, "private key reload error: %s", ERR_error_string(ERR_get_error(), NULL));
        }

        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MasterUnlock();
}
static NS_TLS_SSL_CTX *CertTableGetCtx(const char *cert)
{
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    NS_TLS_SSL_CTX *result = NULL;

    NS_NONNULL_ASSERT(cert != NULL);

    Ns_MasterLock();
    hPtr = Tcl_FirstHashEntry(&certTable, &search);
    while (hPtr != NULL) {
        NS_TLS_SSL_CTX *ctx = (NS_TLS_SSL_CTX *)Tcl_GetHashKey(&certTable, hPtr);
        const char     *storedCert = Tcl_GetHashValue(hPtr);

        if (STREQ(cert, storedCert)) {
            result = ctx;
            break;
        }

        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MasterUnlock();
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ValidationExcpetionExists --
 *
 *      Check whether we can accept the error code in "x509err" based
 *      on the security exception rules provided in
 *      validationExceptionsPtr.
 *
 * Results:
 *      NS_TRUE in case the errorCode is accepted.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
ValidationExcpetionExists(int x509err, NS_SOCKET sock, Ns_DList *validationExceptionsPtr, struct sockaddr *saPtr)
{
    bool      accept = NS_FALSE;
    socklen_t socklen = (socklen_t)sizeof(struct NS_SOCKADDR_STORAGE);
    size_t    specNr;

    NS_NONNULL_ASSERT(validationExceptionsPtr != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);

    /*
     * We have a peer address, we could check it against the addresses
     * in the validation exception rules.
     */
    if (getpeername(sock, saPtr, &socklen) != 0) {
        memset(saPtr, 0, sizeof(socklen));
    }
    Ns_Log(Debug, "??? ValidationExcpetionExists nr validation exceptions %ld", validationExceptionsPtr->size);

    /*
     * We can check the error code against the accepted ones in the validation exceptions.
     */
    for (specNr = 0u; specNr < validationExceptionsPtr->size; specNr ++) {
        NsCertValidationException_t *validationExceptionPtr = validationExceptionsPtr->data[specNr];
        bool                         ipMatch, ruleAccept = NS_FALSE;

        /*
         * Either accept all IP addresses, or check the IP address in
         * the rule whether it matches.
         */
        ipMatch = (validationExceptionPtr->flags & NS_CERT_TRUST_ALL_IPS) != 0
            ? NS_TRUE
            : Ns_SockaddrMaskedMatch(saPtr, (struct sockaddr *)&validationExceptionPtr->mask, (struct sockaddr *)&validationExceptionPtr->ip);

        if (validationExceptionPtr->accept[0] == 0) {
            /*
             * Accept all certificate validation errors from this site.
             */
            ruleAccept = ipMatch;
        } else if (ipMatch) {
            int i;

            /*
             * Check list of accepted exceptions.
             */
            for (i = 0u; i < NS_MAX_VALIDITY_ERRORS_PER_RULE-1; i++) {
                int canAcceptCode = (int)validationExceptionPtr->accept[i];

                if (canAcceptCode == 0) {
                    /*
                     * We reached end of list of accepted errors.
                     */
                    break;
                } else if (canAcceptCode == NS_X509_V_ERR_MATCH_ALL) {
                    ruleAccept = NS_TRUE;
                } else {
                    //ns_inet_ntop((struct sockaddr *)&validationExceptionPtr->ip, ipString, sizeof(ipString));

                    //Ns_Log(Notice, "?????? %d: ip %s x509err %d acceptCode %d (equals %d)",
                    //       i, ipString, x509err, canAcceptCode, canAcceptCode == x509err);
                    if (canAcceptCode == x509err) {
                        ruleAccept = NS_TRUE;
                    }
                }
            }
        }
        Ns_Log(Debug, "??? [%ld] x509err %d flags %.4lx --> accept %d", specNr, x509err, validationExceptionPtr->flags, ruleAccept);

        if (ruleAccept == NS_TRUE) {
            /*
             * No need to check, if other rules might hold as well.
             */
            accept = NS_TRUE;
            break;
        }
    }
    return accept;
}

/*
 *----------------------------------------------------------------------
 *
 * StoreInvalidCertificate --
 *
 *      Store the (invalid) certificate in the filesystem in a folder
 *      (typically named "invalid-certificates"). The filename
 *      consists of a digest of the certificate, followed by the
 *      validation depth and the x509error.
 *
 * Results:
 *      NS_OK for success, NS_ERROR otherwise.
 *
 * Side effects:
 *      Typically writing a file to the filesystem.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
StoreInvalidCertificate(X509 *cert, int x509err, int currentDepth, NsServer *servPtr) {
    Ns_ReturnCode result = NS_OK;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  mdLength = 0;
    struct stat   statInfo;

    if (unlikely(cert == NULL)) {
        Ns_Log(Warning, "cannot obtain invalid certificate from OpenSSL");
        result = NS_ERROR;

    } else if (!Ns_Stat(servPtr->httpclient.invalidCaPath, &statInfo)) {
        Ns_Log(Warning, "StoreInvalidCertificate: invalidCaPath '%s' does not exist", servPtr->httpclient.invalidCaPath);
        result = NS_ERROR;

    } else if (X509_digest(cert, /*EVP_sha1()*/ EVP_sha256(), md, &mdLength) != 1) {
        /*
         * Could not computed the SHA digest of the certificate.
         */
        Ns_Log(Warning, "StoreInvalidCertificate Failed to compute digest of certificate");
        result = NS_ERROR;

    } else {
        Tcl_DString ds, *dsPtr = &ds;
        TCL_SIZE_T  pathLength;

        Ns_Log(Debug, "??? StoreInvalidCertificate digest length %d (max %d), path %s",
               mdLength, EVP_MAX_MD_SIZE, servPtr->httpclient.invalidCaPath);

        /*
         * Build the path for storing the invalid certificate
         */
        Tcl_DStringInit(dsPtr);
        Tcl_DStringAppend(dsPtr, servPtr->httpclient.invalidCaPath, TCL_INDEX_NONE);
        if (dsPtr->string[dsPtr->length-1] != '/') {
            Tcl_DStringAppend(dsPtr, "/", 1);
        }
        pathLength = dsPtr->length;
        Tcl_DStringSetLength(dsPtr, pathLength + (TCL_SIZE_T)mdLength * 2 + 1);

        /*
         * The filename of the PEM file consists of the hex-value of
         * the digest of the certificate, followed by the current
         * validation depth and the SSL error code.
         */
        Ns_HexString(md, dsPtr->string + pathLength, (TCL_SIZE_T)mdLength, NS_FALSE);
        Tcl_DStringSetLength(dsPtr, pathLength+(TCL_SIZE_T)mdLength * 2);
        Ns_DStringPrintf(dsPtr, "-%d-%d.pem", currentDepth, x509err);

        if (Ns_Stat(dsPtr->string, &statInfo)) {
            Ns_Log(Notice, "invalid certificate stored already: %s", dsPtr->string);
        } else {
            /*
             * Save the certificate. We do no care about concurrency
             * here.
             */
            FILE *fp = fopen(dsPtr->string, "w");

            if (fp) {
                if (PEM_write_X509(fp, cert)) {
                    Ns_Log(Security, "saved invalid certificate: %s", dsPtr->string);
                } else {
                    Ns_Log(Warning, "failed to save invalid certificate in: %s", dsPtr->string);
                }
                fclose(fp);
            } else {
                Ns_Log(Warning, "could not open %s for writing", dsPtr->string);
            }
        }

        Tcl_DStringFree(dsPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CertificateValidationCB --
 *
 *      OpenSSL callback invoked to validate a certificate. This function
 *      is called for each certificate in the chain. The parameter
 *      "preverify_ok" indicates whether the certificate has passed the
 *      default validation checks (1 = passed, 0 = failed).
 *
 *      The "ctx" argument is an X509_STORE_CTX structure containing
 *      verification details (e.g., current error, certificate depth, etc.).
 *
 * Results:
 *      Return 1 to accept the certificate, or 0 to reject it.
 *      If 0 is returned, the TLS handshake is aborted.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CertficateValidationCB(int preverify_ok, X509_STORE_CTX *ctx)
{
    int  certificateAccepted = preverify_ok;
    SSL *sslPtr = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());

    /*
     * In case we want to set an error, we could use something like
     *    X509_STORE_CTX_set_error(ctx, X509_V_ERR_CERT_CHAIN_TOO_LONG);
     */

    if (!certificateAccepted && sslPtr != NULL) {
        int       currentDepth = X509_STORE_CTX_get_error_depth(ctx);
        SSL_CTX  *sslCtx       = SSL_get_SSL_CTX(sslPtr);
        NsServer *servPtr      = SSL_CTX_get_ex_data(sslCtx, ClientCtxDataIndex);
        NS_SOCKET sock         = SSL_get_fd(sslPtr);

        Ns_Log(Debug, "??? CertficateValidationCB got socket %d ctx %p currentDepth %d index %d servPtr %p",
               sock, (void*)sslCtx, currentDepth, ClientCtxDataIndex, (void*)servPtr);

        if (unlikely(servPtr == NULL)) {
            Ns_Log(Warning, "CertficateValidationCB cannot determine server");

        } else if (sock != NS_INVALID_SOCKET) {
            struct NS_SOCKADDR_STORAGE sa;
            struct sockaddr           *saPtr = (struct sockaddr *)&sa;
            int x509err = X509_STORE_CTX_get_error(ctx);

            Ns_Log(Debug, "??? CertficateValidationCB servPtr %p '%s' depth: configured %d verify depth %d",
                   (void*)servPtr, servPtr->server, servPtr->httpclient.verify_depth, currentDepth);

            certificateAccepted = ValidationExcpetionExists(x509err, sock, &servPtr->httpclient.validationExceptions, saPtr);
            if (certificateAccepted) {
                char ipString[NS_IPADDR_SIZE];

                ns_inet_ntop(saPtr, ipString, NS_IPADDR_SIZE);
                Ns_Log(Warning, "invalid certificate accepted (%s %s)", ipString, X509_verify_cert_error_string(x509err));
                (void)StoreInvalidCertificate(X509_STORE_CTX_get_current_cert(ctx), x509err, currentDepth, servPtr);
            }

        } else {
            Ns_Log(Warning, "CertficateValidationCB cannot determine peer address, since socket is invalid");
        }
    } else if (sslPtr == NULL) {
        Ns_Log(Warning, "CertficateValidationCB could not obtain SSL pointer");
    }

    Ns_Log(Debug, "??? CertficateValidationCB ===> returns %d (accepted by openssl %d)",
           certificateAccepted, preverify_ok);

    return certificateAccepted;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxServerCreateCfg --
 *
 *      Create and fully configure an OpenSSL SSL_CTX for server‐side TLS,
 *      using the given certificate, CA files, cipher settings, protocol
 *      restrictions, and optional application data.
 *
 * Parameters:
 *      interp       - Tcl interpreter for error messages (must not be NULL)
 *      cert         - Path to server PEM file (certificate + chain + key)
 *      caFile       - Path to CA bundle file for client authentication
 *      caPath       - Path to directory of hashed CA files
 *      verify       - true to require and verify client certificates
 *      ciphers      - OpenSSL cipher list string, or NULL to leave default
 *      ciphersuites - TLS1.3 ciphersuites string, or NULL if unused
 *      protocols    - comma-separated exclusions like "!TLSv1.0,!SSLv3", or NULL
 *      app_data     - Opaque pointer stored in CTX via SSL_CTX_set_app_data
 *      ctxPtr       - Address where the new SSL_CTX* will be returned (non-NULL)
 *
 * Returns:
 *      TCL_OK on success (and *ctxPtr set to the new SSL_CTX)
 *      TCL_ERROR on failure, with an error message left in interp;
 *      the partially configured SSL_CTX is freed to avoid leaks.
 *
 * Side Effects:
 *      - Allocates and configures an OpenSSL SSL_CTX.
 *      - Logs notices for disabled protocols and errors.
 *      - May populate the global certificate table for hot-reload support.
 *
 *----------------------------------------------------------------------
 */
int
Ns_TLS_CtxServerCreateCfg(Tcl_Interp *interp,
                          const char *cert, const char *caFile, const char *caPath,
                          bool verify, const char *ciphers, const char *ciphersuites,
                          const char *protocols, const char *alpn,
                          void *app_data,
                          NS_TLS_SSL_CTX **ctxPtr)
{
    NS_TLS_SSL_CTX   *ctx;
    const SSL_METHOD *server_method;
    int rc;

    NS_NONNULL_ASSERT(ctxPtr != NULL);

    Ns_Log(Debug, "Ns_TLS_CtxServerCreate cert '%s' app_data %p", cert, (void*)app_data);

#ifdef HAVE_OPENSSL_PRE_1_1
    server_method = SSLv23_server_method();
#else
    server_method = TLS_server_method();
#endif

    ctx = SSL_CTX_new(server_method);

    *ctxPtr = ctx;
    if (ctx == NULL) {
        ReportError(interp, "ssl ctx init failed: %s", ERR_error_string(ERR_get_error(), NULL));
        return TCL_ERROR;
    }

    if (cert == NULL && caFile == NULL) {
        ReportError(interp, "At least one of certificate or cafile must be specified!");
        goto fail;
    }

    if (ciphers != NULL) {
        rc = SSL_CTX_set_cipher_list(ctx, ciphers);
        if (rc == 0) {
            ReportError(interp, "ssl ctx invalid cipher list '%s': %s",
                        ciphers, ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
    }

#if !defined(HAVE_OPENSSL_PRE_1_1) && defined(TLS1_3_VERSION) && !defined(OPENSSL_NO_TLS1_3)
    if (ciphersuites != NULL) {
        rc = SSL_CTX_set_ciphersuites(ctx, ciphersuites);
        if (rc == 0) {
            ReportError(interp, "ssl ctx invalid ciphersuites specification '%s': %s",
                        ciphersuites, ERR_error_string(ERR_get_error(), NULL));
        }
    }
#endif

    /*
     * Parse SSL protocols
     */
    {
        unsigned long n = SSL_OP_ALL;

        if (protocols != NULL) {
            if (strstr(protocols, "!SSLv2") != NULL) {
                n |= SSL_OP_NO_SSLv2;
                Ns_Log(Notice, "nsssl: disabling SSLv2");
            }
            if (strstr(protocols, "!SSLv3") != NULL) {
                n |= SSL_OP_NO_SSLv3;
                Ns_Log(Notice, "nsssl: disabling SSLv3");
            }
            if (strstr(protocols, "!TLSv1.0") != NULL) {
                n |= SSL_OP_NO_TLSv1;
                Ns_Log(Notice, "nsssl: disabling TLSv1.0");
            }
            if (strstr(protocols, "!TLSv1.1") != NULL) {
                n |= SSL_OP_NO_TLSv1_1;
                Ns_Log(Notice, "nsssl: disabling TLSv1.1");
            }
#ifdef SSL_OP_NO_TLSv1_2
            if (strstr(protocols, "!TLSv1.2") != NULL) {
                n |= SSL_OP_NO_TLSv1_2;
                Ns_Log(Notice, "nsssl: disabling TLSv1.2");
            }
#endif
#ifdef SSL_OP_NO_TLSv1_3
            if (strstr(protocols, "!TLSv1.3") != NULL) {
                n |= SSL_OP_NO_TLSv1_3;
                Ns_Log(Notice, "nsssl: disabling TLSv1.3");
            }
#endif
        }
        SSL_CTX_set_options(ctx, n);
    }

#if OPENSSL_VERSION_NUMBER >= 0x1000200fL
    {
        Tcl_DString   alpnDs;
        unsigned int *mem;

        Tcl_DStringInit(&alpnDs);

        if (BuildALPNWireFormat(&alpnDs, alpn) == NS_ERROR || alpnDs.length == 0) {
            ReportError(interp, "invalid ALPN protocol string '%s'", alpn);
            Tcl_DStringFree(&alpnDs);
            goto fail;
        }

        mem = ns_malloc(sizeof(unsigned int) + (size_t)alpnDs.length);
        *mem = (unsigned int)alpnDs.length;
        memcpy(mem + 1, alpnDs.string, (size_t)alpnDs.length);
        SSL_CTX_set_alpn_select_cb(ctx, ALPNSelectCB, mem + 1);
        Tcl_DStringFree(&alpnDs);
    }
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    if (app_data != NULL) {
        NsSSLConfig *cfgPtr = app_data;

        if (cfgPtr->tlsKeylogFile != NULL) {
            Ns_Log(Notice, "KeylogCB registered (file name '%s')", cfgPtr->tlsKeylogFile);
            SSL_CTX_set_keylog_callback(ctx, KeylogCB);
        }
    }
#endif

    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_load_verify_locations(ctx, caFile, caPath);
    // SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (app_data != NULL) {
        SSL_CTX_set_app_data(ctx, app_data);
    }

    SSL_CTX_set_default_passwd_cb(ctx, TLSPasswordCB);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *)cert);

    DrainErrorStack(Warning, "Ns_TLS_CtxServerCreate", ERR_get_error());

    if (cert != NULL) {
        /*
         * Load certificate and private key
         */
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            ReportError(interp, "certificate '%s' load chain error: %s",
                        cert, ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
        /*
         * Remember ctx and certificate name for reloading.
         */
        CertTableAdd(ctx, cert);

        if (SSL_CTX_use_PrivateKey_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
            ReportError(interp, "certificate '%s' private key load error: %s",
                        cert, ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
#ifndef OPENSSL_HAVE_DH_AUTO
        /*
         * Get DH parameters from .pem file
         */
        {
            BIO *bio = BIO_new_file(cert, "r");
            DH  *dh  = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
            BIO_free(bio);

            if (dh != NULL) {
                if (SSL_CTX_set_tmp_dh(ctx, dh) < 0) {
                    Ns_Log(Error, "nsssl: Couldn't set DH parameters");
                    return TCL_ERROR;
                }
                DH_free(dh);
            }
        }
#endif

    }

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    SSL_CTX_set_quiet_shutdown(ctx, 1);
#endif

    return TCL_OK;

 fail:
    SSL_CTX_free(ctx);
    *ctxPtr = NULL;

    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxServerCreate --
 *
 *      Backward compatible legacy function calling
 *      Ns_TLS_CtxServerCreateCfg().  The differences are
 *      * app_data (Opaque pointer stored in CTX via
 *        SSL_CTX_set_app_data) can't be provided through this
 *        interface.
 *      * ALPN can't be provided.
 *
 *      One consequence is, the internal NsSSLConfig data can't be
 *      provided over this interface, and the pass-phrase passing via
 *      external script can't be used.
 *
 * Results:
 *      Same as in Ns_TLS_CtxServerCreateCfg().
 *
 * Side effects:
 *      Same as in Ns_TLS_CtxServerCreateCfg().
 *
 *----------------------------------------------------------------------
 */
int
Ns_TLS_CtxServerCreate(Tcl_Interp *interp,
                       const char *cert, const char *caFile, const char *caPath,
                       bool verify, const char *ciphers, const char *ciphersuites,
                       const char *protocols,
                       NS_TLS_SSL_CTX **ctxPtr)
{
    return Ns_TLS_CtxServerCreateCfg(interp, cert, caFile, caPath,
                                     verify, ciphers, ciphersuites,
                                     protocols, "http/1.1",
                                     NULL,
                                     ctxPtr);
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_SSLAccept --
 *
 *      Accept a new SSL/TLS connection by performing the TLS
 *      handshake on an incoming connection. This function wraps the
 *      OpenSSL SSL_accept() call, establishing a secure channel on
 *      the provided socket. If the handshake is successful, the SSL
 *      structure is fully initialized and the connection can be used
 *      for secure communication.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TLS_SSLAccept(Tcl_Interp *interp, NS_SOCKET sock, NS_TLS_SSL_CTX *ctx,
                 NS_TLS_SSL **sslPtr)
{
    NS_TLS_SSL     *ssl;
    int             result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(sslPtr != NULL);

    ssl = SSL_new(ctx);
    *sslPtr = ssl;
    if (ssl == NULL) {
        char *errMsg, errorBuffer[256];

        errMsg = ERR_error_string(ERR_get_error(), errorBuffer);
        Ns_TclPrintfResult(interp, "SSLAccept failed: %s", errMsg);
        Ns_Log(Debug, "SSLAccept failed: %s", errMsg);
        result = TCL_ERROR;

    } else {

        SSL_set_fd(ssl, sock);
        SSL_set_accept_state(ssl);

        for (;;) {
            int rc, err;

            rc = SSL_do_handshake(ssl);
            err = SSL_get_error(ssl, rc);

            if (err == SSL_ERROR_WANT_READ) {
                (void)WaitFor(sock, (unsigned int)NS_SOCK_READ, NULL);
                continue;

            } else if (err == SSL_ERROR_WANT_WRITE) {
                (void)WaitFor(sock, (unsigned int)NS_SOCK_WRITE, NULL);
                continue;
            }
            break;
        }

        if (!SSL_is_init_finished(ssl)) {
            char *errMsg, errorBuffer[256];

            errMsg = ERR_error_string(ERR_get_error(), errorBuffer);
            Ns_TclPrintfResult(interp, "ssl accept failed: %s", errMsg);
            Ns_Log(Debug, "SSLAccept failed: %s", errMsg);

            SSL_free(ssl);
            *sslPtr = NULL;
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SSLRecvBufs2 --
 *
 *      Read data from a nonblocking socket into a vector of buffers.
 *      Ns_SockRecvBufs2() is similar to Ns_SockRecvBufs() with the
 *      following differences:
 *        a) the first argument is an SSL *
 *        b) it performs no timeout handliong
 *        c) it returns the sockstate in its last argument
 *
 * Results:
 *      Number of bytes read or -1 on error.  The return
 *      value will be 0 when the peer has performed an orderly shutdown.
 *      The resulting sockstate has one of the following codes:
 *
 *      NS_SOCK_READ, NS_SOCK_DONE, NS_SOCK_AGAIN, NS_SOCK_EXCEPTION
 *
 * Side effects:
 *      May wait for given timeout if first attempt would block.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SSLRecvBufs2(SSL *sslPtr, struct iovec *bufs, int UNUSED(nbufs),
                Ns_SockState *sockStatePtr, unsigned long *errnoPtr)
{
    ssize_t       nRead = 0;
    int           got = 0, sock, n, err;
    char         *buf = NULL;
    unsigned long sslERRcode = 0u;
    char          errorBuffer[256];
    Ns_SockState  sockState = NS_SOCK_READ;

    NS_NONNULL_ASSERT(sslPtr != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);
    NS_NONNULL_ASSERT(sockStatePtr != NULL);

    buf = (char *)bufs->iov_base;
    sock = SSL_get_fd(sslPtr);

    ERR_clear_error();
    n = SSL_read(sslPtr, buf + got, (int)bufs->iov_len - got);
    err = SSL_get_error(sslPtr, n);
    //Ns_Log(Notice, "=== SSL_read(%d) received:%d, err:%d", sock, n, err);

    switch (err) {
    case SSL_ERROR_NONE:
        if (n < 0) {
            Ns_Log(Debug, "SSL_read(%d) received:%d, but have not SSL_ERROR", sock, n);
            nRead = n;
        } else {
            got += n;
            Ns_Log(Debug, "SSL_read(%d) got:%d", sock, got);
            nRead = got;
        }
        break;

    case SSL_ERROR_ZERO_RETURN:

        Ns_Log(Debug, "SSL_read(%d) ERROR_ZERO_RETURN got:%d", sock, got);

        nRead = got;
        sockState = NS_SOCK_DONE;
        break;

    case SSL_ERROR_WANT_READ:

        Ns_Log(Debug, "SSL_read(%d) ERROR_WANT_READ got:%d", sock, got);

        nRead = got;
        sockState = NS_SOCK_AGAIN;
        break;

    case SSL_ERROR_SYSCALL:

        sslERRcode = ERR_get_error();
        Ns_Log(Debug, "SSL_read(%d) SSL_ERROR_SYSCALL got:%d sslERRcode %lu: %s", sock, got,
               sslERRcode, ERR_error_string(sslERRcode, errorBuffer));

        if (sslERRcode == 0) {
            Ns_Log(Debug, "SSL_read(%d) ERROR_SYSCALL (eod?), got:%d", sock, got);
            nRead = got;
            sockState = NS_SOCK_DONE;
            break;
        } else {
            const char *ioerr;

            ioerr = ns_sockstrerror(ns_sockerrno);
            Ns_Log(Debug, "SSL_read(%d) ERROR_SYSCALL %s", sock, ioerr);
        }
        NS_FALL_THROUGH; /* fall through */

    default:
        sslERRcode = ERR_get_error();

        Ns_Log(Debug, "SSL_read(%d) error handler err %d sslERRcode %lu",
               sock, err, sslERRcode);
        /*
         * Starting with the commit in OpenSSL 1.1.1 branch
         * OpenSSL_1_1_1-stable below, at least HTTPS client requests
         * answered without an explicit content length start to
         * fail. This can be tested with:
         *
         *      ns_logctl severity Debug(task) on
         *      ns_http run https://www.google.com/
         *
         * The fix below just triggers for exactly this condition to
         * provide a graceful end for these requests.
         *
         * https://github.com/openssl/openssl/commit/db943f43a60d1b5b1277e4b5317e8f288e7a0a3a
         */
        if (err == SSL_ERROR_SSL) {
            int reasonCode = ERR_GET_REASON(sslERRcode);

            Ns_Log(Debug, "SSL_read(%d) error handler SSL_ERROR_SSL sslERRcode %lu reason code %d",
                   sock, sslERRcode, reasonCode);
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
            if (reasonCode == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                /*
                 * Only complain loudly, when socket not in init
                 * mode. SSL_in_init() returns 1 if the SSL/TLS state
                 * machine is currently processing or awaiting
                 * handshake messages, or 0 otherwise.
                 */
                Ns_LogSeverity level = (SSL_in_init(sslPtr) == 1 ? Debug : Notice);

                Ns_Log(level, "SSL_read(%d) ERROR_SYSCALL sees UNEXPECTED_EOF_WHILE_READING", sock);
                nRead = got;
                sockState = NS_SOCK_DONE;
                break;
            }
#endif
            if (reasonCode == SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN) {
                Ns_Log(Debug, "SSL_read(%d) client complains: CERTIFICATE_UNKNOWN", sock);
                nRead = 0;
                sockState = NS_SOCK_AGAIN;
                break;
            }
            if (reasonCode == SSL_R_UNSUPPORTED_PROTOCOL) {
                struct NS_SOCKADDR_STORAGE sa;
                socklen_t socklen = (socklen_t)sizeof(sa);
                char      ipString[NS_IPADDR_SIZE];

                if ( getpeername(sock, (struct sockaddr *)&sa, &socklen) == 0) {
                    ns_inet_ntop((struct sockaddr *)&sa, ipString, sizeof(ipString));
                } else {
                    ipString[0] = '\0';
                }
                Ns_Log(Notice, "SSL_read(%d) client requested unsupported protocol: %s from peer %s",
                       sock, SSL_get_version(sslPtr), ipString);
            }
        }
        /*
         * Report all sslERRcodes from the OpenSSL error stack as
         * "notices" in the system log file.
         */
        if (sslERRcode != 0u) {
            Ns_Log(Notice, "SSL_read(%d) error received:%d, got:%d, err:%d",
                   sock, n, got, err);
            DrainErrorStack(Notice, "... SSL_read error", sslERRcode);
        }

        SSL_set_shutdown(sslPtr, SSL_RECEIVED_SHUTDOWN);
        //Ns_Log(Notice, "SSL_read(%d) error after shutdown", sock);
        nRead = -1;
        break;

    }

    if (nRead < 0) {
        sockState = NS_SOCK_EXCEPTION;
        *errnoPtr = sslERRcode;
    }

    *sockStatePtr = sockState;
    Ns_Log(Debug, "### SSL_read(%d) return:%ld sockState:%.2x", sock, nRead, sockState);

    return nRead;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SSLSendBufs2 --
 *
 *      Send a vector of buffers on a nonblocking TLS socket.
 *      It is similar to Ns_SockSendBufs() except that it
 *        a) receives an SSL * as first argument
 *        b) it does not care about partial writes,
 *           it simply returns the number of bytes sent.
 *        c) it never blocks
 *        d) it does not try corking
 *
 * Results:
 *      Number of bytes sent (which might be also 0 on NS_EAGAIN cases)
 *      or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
ssize_t
Ns_SSLSendBufs2(SSL *ssl, const struct iovec *bufs, int nbufs)
{
    ssize_t sent = 0;

    NS_NONNULL_ASSERT(ssl != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);

    if (nbufs > 1) {
        /* sent = -1; to silence bad static checkers (cppcheck), fb infer complains when set */
        Ns_Fatal("Ns_SSLSendBufs2: can handle at most one buffer at the time");

    } else if (bufs[0].iov_len > 0) {
        int  err;

        sent = SSL_write(ssl, bufs[0].iov_base, (int)bufs[0].iov_len);
        err = SSL_get_error(ssl, (int)sent);

        if (err == SSL_ERROR_WANT_WRITE) {
            sent = 0;

        } else if (err == SSL_ERROR_SYSCALL) {
            Ns_Log(Debug, "SSL_write ERROR_SYSCALL %s", ns_sockstrerror(ns_sockerrno));

        } else if (err != SSL_ERROR_NONE) {
            Ns_Log(Debug, "SSL_write: sent:%ld, error:%d", sent, err);
        }
    }

    return sent;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SSLSetErrorCode --
 *
 *      Set the Tcl error code in the interpreter based on the
 *      provided numeric value (OpenSSL error value) and return the
 *      error message.
 *
 * Results:
 *      Error message in form of a string.
 *
 * Side effects:
 *      Sets Tcl error code.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_SSLSetErrorCode(Tcl_Interp *interp, unsigned long sslERRcode)
{
    const char *errorMsg;

    NS_NONNULL_ASSERT(interp != NULL);

    if (ERR_GET_LIB(sslERRcode) == ERR_LIB_SYS) {
        errorMsg = Ns_PosixSetErrorCode(interp, ERR_GET_REASON(sslERRcode));
    } else {
        char errorBuf[256];
        /*
         * Get long human readable error message from OpenSSL (including
         * hex error code, library, and reason string).
         */
        ERR_error_string_n(sslERRcode, errorBuf, sizeof(errorBuf));

        Tcl_SetErrorCode(interp, "OPENSSL", errorBuf, NS_SENTINEL);
        errorMsg = ERR_reason_error_string(sslERRcode);
    }

    return errorMsg;
}


/*
 *----------------------------------------------------------------------
 *
 * NsCertCtlListCmd - subcommand of NsTclCertCtlObjCmd --
 *
 *      Implements "ns_certctl reload" command for listing
 *      certificates. This function retrieves and formats a list of
 *      certificates currently loaded or managed by the server,
 *      returning the information as a Tcl list.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
NsCertCtlListCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_HashEntry  *hPtr;
        Tcl_HashSearch  search;
        Tcl_Obj        *resultListObj = Tcl_NewListObj(0, NULL);
        Tcl_DString     ds;

        Tcl_DStringInit(&ds);
        Ns_MasterLock();
        hPtr = Tcl_FirstHashEntry(&certTable, &search);
        while (hPtr != NULL) {
            Tcl_Obj         *listObj = Tcl_NewListObj(0, NULL);
            NS_TLS_SSL_CTX  *ctx = (NS_TLS_SSL_CTX *)Tcl_GetHashKey(&certTable, hPtr);
            const char      *cert = Tcl_GetHashValue(hPtr);
            X509            *x509 = SSL_CTX_get0_certificate(ctx);
            const ASN1_TIME *notAfter = X509_get0_notAfter(x509);
            int              remaining_days = 0, remaining_seconds = 0, rc;

            Tcl_ListObjAppendElement(interp, listObj,
                                     Tcl_NewStringObj(cert, TCL_INDEX_NONE));
            rc = ASN1_TIME_diff(&remaining_days, &remaining_seconds, NULL, notAfter);
            if (rc == 1) {
                Tcl_ListObjAppendElement(interp, listObj,
                                         Tcl_NewStringObj("remaining_days", 14));
                Ns_DStringPrintf(&ds, "%5.2f",
                                 remaining_days + (remaining_seconds/(60*60*24.0)));
                Tcl_ListObjAppendElement(interp, listObj,
                                         Tcl_NewStringObj(ds.string, ds.length));

                Tcl_ListObjAppendElement(interp, resultListObj, listObj);
                Tcl_DStringSetLength(&ds, 0);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MasterUnlock();
        Tcl_DStringFree(&ds);

        Tcl_SetObjResult(interp, resultListObj);

    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsCertCtlListCmd - subcommand of NsTclCertCtlObjCmd --
 *
 *      Implements the "ns_certctl reload" command for certificate
 *      control.  This function triggers a reload of certificates -
 *      typically in response to a configuration change or an
 *      administrative signal (e.g., SIGHUP).  It scans for updated
 *      certificate files and reloads them into the server's SSL
 *      contexts, ensuring that any changes to certificates are
 *      applied without requiring a server restart.
 *
 * Results:
 *      Returns a standard Tcl result (TCL_OK on success, TCL_ERROR on
 *      failure).
 *
 * Side effects:
 *      May update the certificate store and SSL contexts. Logs error
 *      messages if any certificate reload operations fail.
 *
 *----------------------------------------------------------------------
 */
static int
NsCertCtlReloadCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        void *arg = NULL;

        CertTableReload(arg);
        result = TCL_OK;

    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclICtlObjCmd --
 *
 *      Implements the "ns_cert_ctl" command for certificate control
 *      and management. This command allows administrators to query,
 *      update, or reload certificate-related settings. It parses
 *      subcommands and their arguments, performs the requested
 *      certificate control operations, and returns a standard Tcl
 *      result.
 *
 * Results:
 *      A standard Tcl result indicating success or failure.
 *
 * Side effects:
 *      May update certificate configuration, reload certificates, or
 *      perform other certificate management tasks depending on the
 *      specific subcommand.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCertCtlObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"list",                 NsCertCtlListCmd},
        {"reload",               NsCertCtlReloadCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}



#else

void NsInitOpenSSL(void)
{
    Ns_Log(Notice, "No support for OpenSSL compiled in");
}

/*
 * Dummy stub functions, for the case, when NaviServer is built without
 * OpenSSL support, e.g. when built for the option --without-openssl.
 */

int
Ns_TLS_SSLConnect(Tcl_Interp *interp, NS_SOCKET UNUSED(sock), NS_TLS_SSL_CTX *UNUSED(ctx),
                  const char *UNUSED(sni_hostname), const char **UNUSED(caFile), const char **UNUSED(caPath),
                  const Ns_Time *UNUSED(timeoutPtr), NS_TLS_SSL **UNUSED(sslPtr))
{
    Ns_TclPrintfResult(interp, "SSLCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

int
Ns_TLS_SSLAccept(Tcl_Interp *interp, NS_SOCKET UNUSED(sock), NS_TLS_SSL_CTX *UNUSED(ctx),
                 NS_TLS_SSL **UNUSED(sslPtr))
{
    Ns_TclPrintfResult(interp, "SSLAccept failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

int
Ns_TLS_CtxClientCreate(Tcl_Interp *interp,
                       const char *UNUSED(cert), const char *UNUSED(caFile), const char *UNUSED(caPath), bool UNUSED(verify),
                       NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    Ns_TclPrintfResult(interp, "CtxCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

int
Ns_TLS_CtxServerCreate(Tcl_Interp *interp,
                       const char *UNUSED(cert), const char *UNUSED(caFile), const char *UNUSED(caPath),
                       bool UNUSED(verify), const char *UNUSED(ciphers), const char *UNUSED(ciphersuites),
                       const char *UNUSED(protocols),
                       NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    ReportError(interp, "CtxServerCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}
int
Ns_TLS_CtxServerCreateCfg(Tcl_Interp *interp,
                          const char *UNUSED(cert), const char *UNUSED(caFile), const char *UNUSED(caPath),
                          bool UNUSED(verify), const char *UNUSED(ciphers), const char *UNUSED(ciphersuites),
                          const char *UNUSED(protocols), const char *UNUSED(alpn),
                          void *UNUSED(app_data),
                          NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    ReportError(interp, "CtxServerCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *UNUSED(ctx))
{
    /* dummy stub */
}

int
NsTclCertCtlObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    ReportError(interp, "ns_certctl failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

int
Ns_TLS_CtxServerInit(const char *UNUSED(path), Tcl_Interp *UNUSED(interp),
                     unsigned int UNUSED(flags),
                     void *UNUSED(app_data),
                     NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    return TCL_OK;
}

#endif

/*
 *----------------------------------------------------------------------
 *
 * NsTlsGetParameters --
 *
 *      Check TLS specific parameters and return optionally the
 *      default values.  For insecure requests, set "caFile" and
 *      "caPath" to NULL.  Furthermore, leave an error message in the
 *      interp, when called without an TLS context.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int NsTlsGetParameters(NsInterp *itPtr, bool tlsContext, int insecureInt,
                       const char *cert, const char *caFile, const char *caPath,
                       const char **caFilePtr, const char **caPathPtr)
{
    int         result = TCL_OK;
    Tcl_Interp *interp;
    NsServer   *servPtr;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(caFilePtr != NULL);
    NS_NONNULL_ASSERT(caPathPtr != NULL);

    servPtr = itPtr->servPtr;
    interp = itPtr->interp;

    if (tlsContext) {
        if (caFile == NULL || *caFile == '\0') {
            caFile = servPtr->httpclient.caFile;
        }
        if (caPath == NULL || *caPath == '\0') {
            caPath = servPtr->httpclient.caPath;
        }
        *caFilePtr = (insecureInt == 0) ? caFile : NULL;
        *caPathPtr = (insecureInt == 0) ? caPath : NULL;
    } else if (insecureInt == itPtr->servPtr->httpclient.validateCertificates) {
        Ns_TclPrintfResult(interp, "parameter '-insecure' only allowed on HTTPS connections");
        result = TCL_ERROR;
    } else if (caFile != NULL) {
        Ns_TclPrintfResult(interp, "parameter '-caFile' only allowed on HTTPS connections");
        result = TCL_ERROR;
    } else if (caPath != NULL) {
        Ns_TclPrintfResult(interp, "parameter '-caPath' only allowed on HTTPS connections");
        result = TCL_ERROR;
    } else if (cert != NULL) {
        Ns_TclPrintfResult(interp, "parameter '-cert' only allowed on HTTPS connections");
        result = TCL_ERROR;
    }

    return result;
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
