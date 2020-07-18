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

/*
 * tls.c --
 *
 *      Support for OpenSSL support (SSL and TLS), mostly for HTTPS
 */

#include "nsd.h"

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
    char          *respin;     /* File to load OCSP Response from (or NULL if no file) */
    int            verbose;
    OCSP_RESPONSE *resp;
} SSLCertStatusArg;

static SSLCertStatusArg sslCertStatusArg;
# endif

/*
 * OpenSSL callback functions.
 */
static int SSL_serverNameCB(SSL *ssl, int *al, void *arg);
static DH *SSL_dhCB(SSL *ssl, int isExport, int keyLength);
static int SSLPassword(char *buf, int num, int rwflag, void *userdata);
# ifdef HAVE_OPENSSL_PRE_1_1
static void SSL_infoCB(const SSL *ssl, int where, int ret);
# endif

# ifndef OPENSSL_NO_OCSP
static int SSL_cert_statusCB(SSL *ssl, void *arg);
# endif

/*
 * Local functions defined in this file
 */
static void ReportError(Tcl_Interp *interp, const char *fmt, ...)
    NS_GNUC_NONNULL(2) NS_GNUC_PRINTF(2,3);

static Ns_ReturnCode WaitFor(NS_SOCKET sock, unsigned int st);

# ifndef OPENSSL_NO_OCSP
static int OCSP_FromCacheFile(Tcl_DString *dsPtr, OCSP_CERTID *id, OCSP_RESPONSE **resp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static OCSP_CERTID *OCSP_get_cert_id(const SSL *ssl, X509 *cert)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int OCSP_computeResponse(SSL *ssl, const SSLCertStatusArg *srctx, OCSP_RESPONSE **resp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static OCSP_RESPONSE *OCSP_FromAIA(OCSP_REQUEST *req, const char *aiaURL, int req_timeout)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
#endif

/*
 *----------------------------------------------------------------------
 *
 * Include pre-generated DH parameters
 *
 *----------------------------------------------------------------------
 */
#ifndef HEADER_DH_H
# include <openssl/dh.h>
#endif
static DH *get_dh512(void);
static DH *get_dh1024(void);

#if defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x20700000L
/*
 * Compatibility function for libressl < 2.7; DH_set0_pqg is used just by the
 * Diffie-Hellman parameters in dhparams.h.
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
#endif /* LIBRESSL_VERSION_NUMBER */

#include "dhparams.h"


/*
 *----------------------------------------------------------------------
 * Callback implementations.
 *----------------------------------------------------------------------
 *
 * Callback used for ephemeral DH keys
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

#ifdef HAVE_OPENSSL_PRE_1_1
/*
 * The renegotiation issue was fixed in recent versions of OpenSSL,
 * and the flag was removed, therefore, this function is just for
 * compatibility with old version of OpenSSL (flag removed in OpenSSL
 * 1.1.*).
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

        //ctx = SSL_get_SSL_CTX(ssl);
        //cfgPtr = (NsSSLConfig *) SSL_CTX_get_app_data(ctx);

        /*
         * The default for *al is initialized by SSL_AD_UNRECOGNIZED_NAME = 112.
         * Find info about these codes via:
         *    fgrep -r --include=*.h 112 /usr/local/src/openssl/ | fgrep AD
         */
        Ns_Log(Notice, "SSL_serverNameCB got server name <%s> al %d doSNI %d",
               serverName, (al != NULL ? *al : 0), doSNI);

        /*
         * Perform lookup from host table only, when doSNI is true
         * (i.e. when per virtual server certificates were specified.
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

            ctx = NsDriverLookupHostCtx(ds.string, sockPtr->driver);

            Ns_Log(Notice, "SSL_serverNameCB lookup of <%s> location %s port %hu -> %p",
                   serverName, ds.string, port, (void*)ctx);

            /*
             * When the lookup succeeds, we have the alternate SSL_CTX
             * that we will use. Otherwise, do not acknowledge the
             * servername request.  Return the same value as when not
             * servername was provided (SSL_TLSEXT_ERR_NOACK).
             */
            if (ctx != NULL) {
                Ns_Log(Notice, "SSL_serverNameCB switches server context");
                SSL_set_SSL_CTX(ssl, ctx);
                result = SSL_TLSEXT_ERR_OK;
            }
            Tcl_DStringFree(&ds);
        }
    }

    return result;
}

#ifndef OPENSSL_NO_OCSP
static int SSL_cert_statusCB(SSL *ssl, void *arg)
{
    SSLCertStatusArg *srctx = arg;
    int               result = SSL_TLSEXT_ERR_ALERT_FATAL;
    OCSP_RESPONSE    *resp = NULL;
    unsigned char    *rspder = NULL;
    int               rspderlen;

    if (srctx->verbose) {
        Ns_Log(Notice, "cert_status: callback called");
    }
    Ns_Log(Notice, "=== cert_status: callback called");

    /*
     * If we have not in-memory cached the OCSP response yet, fetch the value
     * either form the disk cache or from the URL provided via the DER encoded
     * OCSP request.
     */
    if (srctx->resp == NULL) {

        result = OCSP_computeResponse(ssl, srctx, &resp);
        if (result != SSL_TLSEXT_ERR_OK) {
            if (resp != NULL) {
                OCSP_RESPONSE_free(resp);
            }
            goto err;
        }
        /*
         * Perform in-memory caching of the OCSP_RESPONSE.
         */
        srctx->resp = resp;
    } else {
        resp = srctx->resp;
    }

    rspderlen = i2d_OCSP_RESPONSE(resp, &rspder);

    Ns_Log(Notice, "cert_status: callback returns OCSP_RESPONSE with length %d", rspderlen);
    if (rspderlen <= 0) {
        if (resp != NULL) {
            OCSP_RESPONSE_free(resp);
            srctx->resp = NULL;
        }
        goto err;
    }

    SSL_set_tlsext_status_ocsp_resp(ssl, rspder, rspderlen);
    if (srctx->verbose) {
        Ns_Log(Notice, "cert_status: OCSP response sent to client");
        //OCSP_RESPONSE_print(bio_err, resp, 2);
    }
    result = SSL_TLSEXT_ERR_OK;

 err:
    if (result != SSL_TLSEXT_ERR_OK) {
        //ERR_print_errors(bio_err);
    }

    //OCSP_RESPONSE_free(resp);

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
 *      None
 *
 *----------------------------------------------------------------------
 */
static OCSP_CERTID *
OCSP_get_cert_id(const SSL *ssl, X509 *cert)
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
    }

    if (store_ctx != NULL) {
        X509_STORE_CTX_free(store_ctx);
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
 *      Tcl result code (NS_OK, NS_CONTINUE, NS_ERROR).  NS_CONTINUE means
 *      that there is no cache entry yet, but the filename of the cache file
 *      is returned in the first argument.
 *
 * Side effects:
 *      None
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

        Tcl_DStringInit(&outputBuffer);
        Tcl_DStringSetLength(&outputBuffer, pserial->length*1 + 1);

        Ns_HexString(pserial->data, outputBuffer.string, pserial->length, NS_TRUE);
        /*
         *
         */
        if (Ns_HomePathExists("logs", (char *)0L)) {
            struct stat fileInfo;
            const char *fileName;

            /*
             * A result of TCL_CONTINUE or TCL_OK implies a computed filename
             * of the cache file in dsPtr;
             */
            Tcl_DStringAppend(&outputBuffer, ".der", 4);
            fileName = Ns_HomePath(dsPtr, "logs", "/", outputBuffer.string, (char *)0L);
            result = TCL_CONTINUE;

            if (Ns_Stat(dsPtr->string, &fileInfo)) {
                BIO *derbio;

                // we could/should check here a validity time based on mtime
                /*fprintf(stderr, "... file %s exists (%ld bytes)\n",
                  fileName, (long)fileInfo.st_size);*/

                derbio = BIO_new_file(fileName, "rb");
                if (derbio == NULL) {
                    Ns_Log(Warning, "cert_status: Cannot open OCSP response file: %s", fileName);

                } else {
                    *resp = d2i_OCSP_RESPONSE_bio(derbio, NULL);
                    BIO_free(derbio);
                    if (*resp == NULL) {
                        Ns_Log(Warning, "cert_status: Error reading OCSP response file: %s", fileName);
                    } else {
                        result = TCL_OK;
                    }
                }
            } else {
                Ns_Log(Warning, "OCSP cache file does not exist: %s", fileName);
                result = TCL_CONTINUE;
            }
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
 *      Get OCSP_RESPONSE either from a cache file or from the cerificate
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
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
OCSP_computeResponse(SSL *ssl, const SSLCertStatusArg *srctx, OCSP_RESPONSE **resp)
{
    X509           *cert = NULL;
    OCSP_CERTID    *id = NULL;
    OCSP_REQUEST   *req = NULL;
    int             rc, result = SSL_TLSEXT_ERR_NOACK;
    Tcl_DString     cachedResponseFile;
    STACK_OF(OPENSSL_STRING) *aia = NULL; /* Authority Information Access (AIA) Extension */

    NS_NONNULL_ASSERT(ssl != NULL);
    NS_NONNULL_ASSERT(srctx != NULL);
    NS_NONNULL_ASSERT(resp != NULL);

    Tcl_DStringInit(&cachedResponseFile);

    cert = SSL_get_certificate(ssl);
    id = OCSP_get_cert_id(ssl, cert);
    if (id == NULL) {
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

        aia = X509_get1_ocsp(cert);
        if (aia != NULL && srctx->verbose) {
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
                goto err;
            }
        }
        *resp = OCSP_FromAIA(req, sk_OPENSSL_STRING_value(aia, 0),
                             srctx->timeout);
        if (*resp == NULL) {
            Ns_Log(Warning, "cert_status: error querying responder");
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
        goto done;
    }

 err:
    result = SSL_TLSEXT_ERR_ALERT_FATAL;
 done:
    /*
     * If we parsed AIA we need to free
     */
    if (aia != NULL) {
        X509_email_free(aia);
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
 *      Get OCSP_RESPONSE from the cerificate issuing server (Authority
 *      Information Access) via the DER encoded OCSP request.
 *
 * Results:
 *      OCSP_RESPONSE * or NULL in case of failure.
 *
 * Side effects:
 *      Issue http/https request to the AIA server.
 *
 *----------------------------------------------------------------------
 */

static OCSP_RESPONSE *
OCSP_FromAIA(OCSP_REQUEST *req, const char *aiaURL, int req_timeout)
{
    OCSP_RESPONSE *rsp = NULL;
    int            derLength;

    NS_NONNULL_ASSERT(req != NULL);
    NS_NONNULL_ASSERT(aiaURL != NULL);

    Ns_Log(Notice, "OCSP_FromAIA url <%s> timeout %d", aiaURL, req_timeout);

    /*
     * We have already the OCSP request int *req with the ID to be queried
     * filled in.
     */

    derLength = i2d_OCSP_REQUEST(req, NULL);
    if (derLength <= 0) {
        Ns_Log(Error, "cert_status: invalid OCSP request size");

    } else {
        Tcl_DString dsBinary, dsBase64, dsCMD;
        unsigned char *ppout;
        size_t base64len;

        Tcl_DStringInit(&dsBinary);
        Tcl_DStringInit(&dsBase64);
        Tcl_DStringInit(&dsCMD);

        Tcl_DStringSetLength(&dsBinary, derLength + 1);
        ppout = (unsigned char *)dsBinary.string;
        derLength = i2d_OCSP_REQUEST(req, &ppout);

        /*
         * Append DER encoding of the OCSP request via URL-encoding of base64
         * encoding, as defined in https://tools.ietf.org/html/rfc6960#appendix-A
         */
        base64len = MAX(4, ((size_t)derLength * 4/3) + 4);
        Tcl_DStringSetLength(&dsBase64, (int)base64len);
        (void) Ns_Base64Encode((unsigned char *)dsBinary.string,
                               (size_t)derLength, dsBase64.string,
                               0, 0);
        Tcl_DStringAppend(&dsCMD, "ns_http run ", -1);
        Tcl_DStringAppend(&dsCMD, aiaURL, -1);

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
            Tcl_Interp *interp = Ns_TclAllocateInterp(NULL);

            if (interp != NULL) {
                Tcl_DString dsResult;

                Tcl_DStringInit(&dsResult);

                Ns_Log(Notice, "OCSP command: %s\n", dsCMD.string);
                if (Tcl_EvalEx(interp, dsCMD.string, dsCMD.length, 0) != TCL_OK) {
                    Ns_Log(Error, "OCSP_REQUEST '%s' returned error", dsCMD.string);
                } else {
                    Tcl_Obj *resultObj;
                    Tcl_Obj *statusObj = Tcl_NewStringObj("status", -1);
                    Tcl_Obj *bodyObj = Tcl_NewStringObj("body", -1);
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
                            /*fprintf(stderr, "### OCSP_REQUEST status <%s>\n", stringValue);*/
                            status = NS_ERROR;
                        }
                    } else {
                        Ns_Log(Warning, "OCSP_REQUEST: dict has no 'status' %s",
                               Tcl_GetString(resultObj));
                        status = NS_ERROR;
                    }
                    if (status == NS_OK) {
                        if (Tcl_DictObjGet(interp, resultObj, bodyObj, &valueObj) == TCL_OK) {
                            int                  length;
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


#endif



/*
 *----------------------------------------------------------------------
 *
 * NsOpenSSLInit --
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
void NsInitOpenSSL(void)
{
# ifdef HAVE_OPENSSL_EVP_H
    static int initialized = 0;

    if (!initialized) {
#  if OPENSSL_VERSION_NUMBER < 0x10100000L
        CRYPTO_set_mem_functions(ns_malloc, ns_realloc, ns_free);
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
#   else
        OPENSSL_init_ssl(0, NULL);
#   endif
#  endif
        initialized = 1;
        Ns_Log(Notice, "%s initialized", SSLeay_version(SSLEAY_VERSION));
    }
# endif
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxClientCreate --
 *
 *   Create and Initialize OpenSSL context
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */

int
Ns_TLS_CtxClientCreate(Tcl_Interp *interp,
                       const char *cert, const char *caFile, const char *caPath, bool verify,
                       NS_TLS_SSL_CTX **ctxPtr)
{
    NS_TLS_SSL_CTX *ctx;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctxPtr != NULL);

    ctx = SSL_CTX_new(SSLv23_client_method());
    *ctxPtr = ctx;
    if (ctx == NULL) {
        char errorBuffer[256];

        Ns_TclPrintfResult(interp, "ctx init failed: %s", ERR_error_string(ERR_get_error(), errorBuffer));
        return TCL_ERROR;
    }

    SSL_CTX_set_default_verify_paths(ctx);
    if (caFile != NULL || caPath != NULL) {
        SSL_CTX_load_verify_locations(ctx, caFile, caPath);
    }
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (cert != NULL) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            char errorBuffer[256];

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
 *   Free OpenSSL context
 *
 * Results:
 *   none
 *
 * Side effects:
 *  None
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
 *   Wait 10ms (currently hardcoded) for a state change on a socket.
 *   This is used for handling OpenSSL's states SSL_ERROR_WANT_READ
 *   and SSL_ERROR_WANT_WRITE.
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *   None
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
WaitFor(NS_SOCKET sock, unsigned int st)
{
    Ns_Time timeout = { 0, 10000 }; /* 10ms */
    return Ns_SockTimedWait(sock, st, &timeout);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_SSLConnect --
 *
 *   Initialize a socket as ssl socket and wait until the socket is
 *   usable (is connected, handshake performed)
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *   None
 *
 *----------------------------------------------------------------------
 */
int
Ns_TLS_SSLConnect(Tcl_Interp *interp, NS_SOCKET sock, NS_TLS_SSL_CTX *ctx,
                  const char *sni_hostname,
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
        Ns_TclPrintfResult(interp, "SSLCreate failed: %s", ERR_error_string(ERR_get_error(), NULL));
        result = TCL_ERROR;

    } else {
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
            int sslRc, err;

            Ns_Log(Debug, "ssl connect on sock %d", sock);
            sslRc = SSL_connect(ssl);
            err   = SSL_get_error(ssl, sslRc);
            //fprintf(stderr, "### ssl connect sock %d returned err %d\n", sock, err);

            if (err == SSL_ERROR_WANT_READ) {
                (void)WaitFor(sock, (unsigned int)NS_SOCK_READ);
                continue;

            } else if (err == SSL_ERROR_WANT_WRITE) {
                (void)WaitFor(sock, (unsigned int)NS_SOCK_WRITE);
                continue;
            }
            break;
        }

        if (!SSL_is_init_finished(ssl)) {
            Ns_TclPrintfResult(interp, "ssl connect failed: %s", ERR_error_string(ERR_get_error(), NULL));
            result = TCL_ERROR;
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
 * SSLPassword --
 *
 *      Get the SSL password from the console (used by the OpenSSLs
 *      default_passwd_cb)
 *
 * Results:
 *      Length of the password.
 *
 * Side effects:
 *      Password passed back in buf.
 *
 *----------------------------------------------------------------------
 */

static int
SSLPassword(char *buf, int num, int UNUSED(rwflag), void *UNUSED(userdata))
{
    const char *pwd;

    fprintf(stdout, "Enter SSL password:");
    pwd = fgets(buf, num, stdin);
    return (pwd != NULL ? (int)strlen(buf) : 0);
}

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

/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxServerInit --
 *
 *   Read config information, vreate and initialize OpenSSL context.
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */
NsSSLConfig *
NsSSLConfigNew(const char *path)
{
    NsSSLConfig *cfgPtr;

    cfgPtr = ns_calloc(1, sizeof(NsSSLConfig));
    cfgPtr->deferaccept = Ns_ConfigBool(path, "deferaccept", NS_FALSE);
    cfgPtr->verify = Ns_ConfigBool(path, "verify", 0);
    return cfgPtr;
}

int
Ns_TLS_CtxServerInit(const char *path, Tcl_Interp *interp,
                     unsigned int flags,
                     void *app_data,
                     NS_TLS_SSL_CTX **ctxPtr)
{
    int         result;
    const char *cert;

    cert = Ns_ConfigGetValue(path, "certificate");
    Ns_Log(Notice, "=== load certificate from <%s>", path);

    if (cert == NULL) {
        Ns_Log(Error, "nsssl: certificate parameter must be specified in the configuration file under %s", path);
        result = NS_ERROR;
    } else {
        const char *ciphers, *ciphersuites, *protocols;

        ciphers      = Ns_ConfigGetValue(path, "ciphers");
        ciphersuites = Ns_ConfigGetValue(path, "ciphersuites");
        protocols    = Ns_ConfigGetValue(path, "protocols");

        Ns_Log(Debug, "Ns_TLS_CtxServerInit calls Ns_TLS_CtxServerCreate with app data %p",
               (void*) app_data);

        result = Ns_TLS_CtxServerCreate(interp, cert,
                                        NULL /*caFile*/, NULL /*caPath*/,
                                        Ns_ConfigBool(path, "verify", 0),
                                        ciphers, ciphersuites, protocols,
                                        ctxPtr);
        if (result == TCL_OK) {
            NsSSLConfig *cfgPtr = (NsSSLConfig *)app_data;

            Ns_Log(Debug, "Ns_TLS_CtxServerInit ctx %p ctx app %p",
                   (void*)*ctxPtr, SSL_CTX_get_app_data(*ctxPtr));

            if (app_data == NULL && SSL_CTX_get_app_data(*ctxPtr) == NULL) {
                /*
                 * Create new app_data (= NsSSLConfig).
                 */
                cfgPtr = NsSSLConfigNew(path);
                cfgPtr->ctx = *ctxPtr;
                Ns_Log(Debug, "Ns_TLS_CtxServerInit created new app data %p for cert <%s> ctx %p",
                        (void*)cfgPtr, cert, (void*)(cfgPtr->ctx));
                app_data = (void *)cfgPtr;
            }

            if (app_data != NULL) {
                Ns_Log(Debug, "Ns_TLS_CtxServerInit set app data %p ctx %p for cert <%s>",
                        (void*) app_data, (void*)*ctxPtr, cert);
                SSL_CTX_set_app_data(*ctxPtr, app_data);
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
             * Obsolete since 1.1.0 but also supported in 3.0
             */
            SSL_CTX_set_options(*ctxPtr, SSL_OP_SSLEAY_080_CLIENT_DH_BUG);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_TLS_D5_BUG);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_TLS_BLOCK_PADDING_BUG);

            if ((flags & NS_DRIVER_SNI) != 0) {
                SSL_CTX_set_tlsext_servername_callback(*ctxPtr, SSL_serverNameCB);
                /* SSL_CTX_set_tlsext_servername_arg(cfgPtr->ctx, app_data); // not really needed */
            }

            cfgPtr->dhKey512 = get_dh512();
            cfgPtr->dhKey1024 = get_dh1024();
            cfgPtr->dhKey2048 = get_dh2048();
            SSL_CTX_set_tmp_dh_callback(*ctxPtr, SSL_dhCB);

            {
                X509_STORE *storePtr;
                int rc;
                /*
                 * Initialize cert storage for the SSL_CTX; otherwise
                 * X509_STORE_CTX_get_* operations will fail.
                 */
#ifdef SSL_CTX_build_cert_chain
                if (SSL_CTX_build_cert_chain(*ctxPtr, 0) != 1) {
                    Ns_Log(Notice, "nsssl SSL_CTX_build_cert_chain failed");
                }
#endif
                storePtr = SSL_CTX_get_cert_store(*ctxPtr /*SSL_get_SSL_CTX(s)*/);
                Ns_Log(Notice, "nsssl:SSL_CTX_get_cert_store %p", (void*)storePtr);
                rc = X509_STORE_load_locations(storePtr, cert, NULL);
                Ns_Log(Notice, "nsssl:X509_STORE_load_locations %d", rc);
            }
#ifndef OPENSSL_NO_OCSP
            if (Ns_ConfigBool(path, "ocspstapling", NS_FALSE)) {

                memset(&sslCertStatusArg, 0, sizeof(sslCertStatusArg));
                sslCertStatusArg.timeout = -1;
                sslCertStatusArg.verbose = 1;

                SSL_CTX_set_tlsext_status_cb(*ctxPtr, SSL_cert_statusCB);
                SSL_CTX_set_tlsext_status_arg(*ctxPtr, &sslCertStatusArg);
            }
#endif

#if OPENSSL_VERSION_NUMBER > 0x00908070 && !defined(OPENSSL_NO_EC)
            /*
             * Generate key for eliptic curve cryptography (potentially used
             * for Elliptic Curve Digital Signature Algorithm (ECDSA) and
             * Elliptic Curve Diffie-Hellman (ECDH).
             */
            {
                EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);

                if (ecdh == NULL) {
                    Ns_Log(Error, "nsssl: Couldn't obtain ecdh parameters");
                    return NS_ERROR;
                }
                SSL_CTX_set_options(cfgPtr->ctx, SSL_OP_SINGLE_ECDH_USE);
                if (SSL_CTX_set_tmp_ecdh(cfgPtr->ctx, ecdh) != 1) {
                    Ns_Log(Error, "nsssl: Couldn't set ecdh parameters");
                    return NS_ERROR;
                }
                EC_KEY_free (ecdh);
            }
#endif
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxServerCreate --
 *
 *   Create and Initialize OpenSSL context
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *  None
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
    NS_TLS_SSL_CTX   *ctx;
    const SSL_METHOD *server_method;
    int rc;

    NS_NONNULL_ASSERT(ctxPtr != NULL);

#ifdef HAVE_OPENSSL_PRE_1_0_2
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

    if (ciphersuites != NULL) {
        rc = SSL_CTX_set_ciphersuites(ctx, ciphersuites);
        if (rc == 0) {
            ReportError(interp, "ssl ctx invalid ciphersuites specification '%s': %s",
                        ciphersuites, ERR_error_string(ERR_get_error(), NULL));
        }
    }

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

    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_load_verify_locations(ctx, caFile, caPath);
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    SSL_CTX_set_default_passwd_cb(ctx, SSLPassword);

    if (cert != NULL) {
        /*
         * Load certificate and private key
         */
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            ReportError(interp, "certificate load error: %s", ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }

        if (SSL_CTX_use_PrivateKey_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
            ReportError(interp, "private key load error: %s", ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
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
                    return NS_ERROR;
                }
                DH_free(dh);
            }
        }
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
 * Ns_TLS_SSLAccept --
 *
 *   Initialize a socket as ssl socket and wait until the socket
 *   is usable (is accepted, handshake performed)
 *
 * Results:
 *   Tcl result code.
 *
 * Side effects:
 *   None
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
                (void)WaitFor(sock, (unsigned int)NS_SOCK_READ);
                continue;

            } else if (err == SSL_ERROR_WANT_WRITE) {
                (void)WaitFor(sock, (unsigned int)NS_SOCK_WRITE);
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
 *      Ns_SockRecvBufs2() is similar to Ns_SockRecvBufs() with the following
 *      differences:
 *        a) the first argument is a SSL *
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
                Ns_SockState *sockStatePtr)
{
    ssize_t       nRead = 0;
    int           got = 0, sock, n = 0, err = SSL_ERROR_NONE;
    char         *buf = NULL;
    unsigned long sslError;
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

        sslError = ERR_get_error();
        Ns_Log(Debug, "SSL_read(%d) SSL_ERROR_SYSCALL got:%d sslError %lu: %s", sock, got,
               sslError, ERR_error_string(sslError, errorBuffer));

        if (sslError == 0) {
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
        sslError = ERR_get_error();

        Ns_Log(Debug, "SSL_read(%d) error handler err %d sslError %lu",
               sock, err, sslError);
        /*
         * Starting with the commit in OpenSSL 1.1.1 branch
         * OpenSSL_1_1_1-stable below, at least https client requests
         * answered without an explicit content length start to
         * fail. This can be tested with:
         *
         *       ns_logctl severity Debug(task) on
         *       ns_http run https://www.google.com/
         *
         * The fix below just triggers for exactly this condition to
         * provide a graceful end for these requests.
         *
         * https://github.com/openssl/openssl/commit/db943f43a60d1b5b1277e4b5317e8f288e7a0a3a
         */
        if (err == SSL_ERROR_SSL) {
            int reasonCode = ERR_GET_REASON(sslError);

            Ns_Log(Debug, "SSL_read(%d) error handler SSL_ERROR_SSL sslError %lu reason code %d",
                   sock, sslError, reasonCode);
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
            if (reasonCode == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                Ns_Log(Notice, "SSL_read(%d) ERROR_SYSCALL sees UNEXPECTED_EOF_WHILE_READING", sock);
                nRead = got;
                sockState = NS_SOCK_DONE;
                break;
            }
#endif
        }
        /*
         * Report all sslErrors from the OpenSSL error stack as
         * "notices" in the system log file.
         */
        while (sslError != 0u) {
            Ns_Log(Notice, "SSL_read(%d) error received:%d, got:%d, err:%d,"
                   " get_error:%lu, %s", sock, n, got, err, sslError,
                   ERR_error_string(sslError, errorBuffer));
            sslError = ERR_get_error();
        }

        SSL_set_shutdown(sslPtr, SSL_RECEIVED_SHUTDOWN);
        //Ns_Log(Notice, "SSL_read(%d) error after shutdown", sock);
        nRead = -1;
        break;

    }

    if (nRead < 0) {
        sockState = NS_SOCK_EXCEPTION;
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
 *        a) receives a SSL * as first argument
 *        b) it does not care about partial writes,
 *           it simply returns the number of bytes sent.
 *        c) it never blocks
 *        d) it does not try corking
 *
 * Results:
 *      Number of bytes sent (which might be also 0 on EAGAIN cases)
 *      or -1 on error.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
ssize_t

Ns_SSLSendBufs2(SSL *ssl, const struct iovec *bufs, int nbufs)
{
    ssize_t sent = -1;

    NS_NONNULL_ASSERT(ssl != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);

    if (nbufs > 1) {
        Ns_Fatal("Ns_SSLSendBufs2: can handle at most one buffer at the time");
    } else if (bufs[0].iov_len == 0) {
        sent = 0;
    } else {
        int  err;

        sent = SSL_write(ssl, bufs[0].iov_base, (int)bufs[0].iov_len);
        err = SSL_get_error(ssl, (int)sent);

        if (err == SSL_ERROR_WANT_WRITE) {
            sent = 0;
        } else if (err == SSL_ERROR_SYSCALL) {
            const char *ioerr;

            ioerr = ns_sockstrerror(ns_sockerrno);
            Ns_Log(Debug, "SSL_write ERROR_SYSCALL %s", ioerr);
        } else if (err != SSL_ERROR_NONE) {
            Ns_Log(Debug, "SSL_write: sent:%ld, error:%d", sent, err);
        }
    }

    return sent;
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
                  const char *UNUSED(sni_hostname),
                  NS_TLS_SSL **UNUSED(sslPtr))
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

void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *UNUSED(ctx))
{
    /* dummy stub */
}
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
