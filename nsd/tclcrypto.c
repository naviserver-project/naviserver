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
 * tclcrypto.c --
 *
 *      Function callable from Tcl to use OpenSSL crypto support
 */

/*
 * We define for the time being that we want to use an API compatible
 * with OpenSSL 1.1.0.  OpenSSL defines two versions, a hex version
 *
 *   #define OPENSSL_API_COMPAT 0x10100000L
 *
 * and a decimal variant, which should be apparently used in versions
 * beyond OpenSSL 1.1.x.
 */
# define OPENSSL_API_COMPAT 10000

#include "nsd.h"

#ifdef HAVE_OPENSSL_EVP_H
# include "nsopenssl.h"
#endif

/*
 * We need OpenSSL least in version 1.0 or newer for the crypto
 * functions.
 */
#if defined(HAVE_OPENSSL_EVP_H) && !defined(HAVE_OPENSSL_PRE_1_0)

# include <openssl/err.h>
# include <openssl/evp.h>
# include <openssl/rand.h>

# ifdef HAVE_OPENSSL_HKDF
#  include <openssl/kdf.h>
# endif

# ifdef HAVE_OPENSSL_3
#  include <openssl/core_names.h>
#  include <openssl/provider.h>
#  include <openssl/param_build.h>
# endif

/*
 * Data structure local to this file.
 */
typedef struct NsDigest {
    const EVP_MD *md;
# ifdef HAVE_OPENSSL_3
    EVP_MD       *fetchedMd;
# endif
} NsDigest;

typedef struct {
    void        *ptr;
    Ns_FreeProc *freeProc;
} Ns_OsslTmp;

typedef enum {
    NS_DIGEST_USAGE_MD          = 1 << 0,
    NS_DIGEST_USAGE_HMAC        = 1 << 1,
    NS_DIGEST_USAGE_HKDF        = 1 << 2,
    NS_DIGEST_USAGE_PBKDF2      = 1 << 3,
    NS_DIGEST_USAGE_SIGN_VERIFY = 1 << 4
} NsDigestUsage;

typedef enum {
    NS_CRYPTO_CAP_NONE      = 0u,
    NS_CRYPTO_CAP_KEYMGMT   = 1u << 0, /* key can be generated/imported/exported */
    NS_CRYPTO_CAP_SIGNATURE = 1u << 1, /* sign/verify */
    NS_CRYPTO_CAP_AGREEMENT = 1u << 2, /* derive/key exchange */
    NS_CRYPTO_CAP_KEM       = 1u << 3  /* encapsulate/decapsulate */
} Ns_CryptoCapabilities;

typedef enum {
    NS_CRYPTO_KEYGEN_USAGE_ANY = 0,
    NS_CRYPTO_KEYGEN_USAGE_SIGNATURE,
    NS_CRYPTO_KEYGEN_USAGE_KEM,
    NS_CRYPTO_KEYGEN_USAGE_AGREEMENT
} NsCryptoKeygenUsage;

typedef enum {
    NS_CRYPTO_KEYIMPORT_PUBLIC = 1,
    NS_CRYPTO_KEYIMPORT_PRIVATE,
    NS_CRYPTO_KEYIMPORT_KEYPAIR
} Ns_CryptoKeyImportSelection;

typedef enum {
    OUTPUT_FORMAT_RAW    = 0,
    OUTPUT_FORMAT_PEM    = 1,
    OUTPUT_FORMAT_DER    = 2
} OutputFormats;

typedef struct {
    Tcl_Obj       *listObj;
    NsDigestUsage  usage;
} NsDigestListCtx;

typedef struct {
    Tcl_Obj           *listObj;
    NsCryptoKeygenUsage usage;
} NsKeygenListCtx;

/*
 * Static functions defined in this file.
 */
static int DigestGet(Tcl_Interp *interp, const char *digestName, NsDigestUsage usage, NsDigest *digestPtr)
    NS_GNUC_NONNULL(1,2,4);

static void DigestFree(NsDigest *digestPtr)
    NS_GNUC_NONNULL(1);

# if !defined(HAVE_OPENSSL_PRE_1_0) && !defined(HAVE_OPENSSL_3)
static void DigestListCallback(const EVP_MD *m, const char *from, const char *to, void *arg);
# endif


static BIO * PemOpenWriteStream(Tcl_Interp *interp, const char *outfileName)
    NS_GNUC_NONNULL(1);

static int PemWriteResult(Tcl_Interp *interp, BIO *bio, const char *outfileName, const char *what)
    NS_GNUC_NONNULL(1,2,4);

static BIO *PemOpenReadStream(const char *fnOrData)
    NS_GNUC_NONNULL(1);


static void SetResultFromOsslError(Tcl_Interp *interp, const char *prefix)
    NS_GNUC_NONNULL(1,2);

static int SetResultFromMemBio(Tcl_Interp *interp, BIO *bio, const char *what)
    NS_GNUC_NONNULL(1,2,3);

static int SetResultFromRawPublicKey(Tcl_Interp *interp, EVP_PKEY *pkey, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2);

static int SetResultFromRawPrivateKey(Tcl_Interp *interp, EVP_PKEY *pkey, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2);


static int PkeyPublicWrite(Tcl_Interp *interp, EVP_PKEY *pkey, const char *outfileName, bool wantPem)
    NS_GNUC_NONNULL(1,2);

static EVP_PKEY *PkeyGetAnyFromPem(Tcl_Interp *interp, const char *pem, const char *passPhrase)
    NS_GNUC_NONNULL(1,2);

static EVP_PKEY *PkeyGetFromPem(Tcl_Interp *interp, const char *pemFileName, const char *passPhrase, bool private)
    NS_GNUC_NONNULL(1,2);

static int PkeyPublicPemWrite(Tcl_Interp *interp, EVP_PKEY *pkey, const char *what, const char *resultWhat,
                              const char *outfileName)
    NS_GNUC_NONNULL(1,2,3,4);

static Tcl_Obj *PkeyTypeNameObj(Tcl_Interp *interp, EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1,2);

static bool PkeyIsType(EVP_PKEY *pkey, const char *name, int legacyId)
    NS_GNUC_NONNULL(1,2);

static bool PkeySupportsKem(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static bool PkeySupportsAgreement(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static bool PkeySupportsSignature(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static bool PkeyMatchesPrefix(EVP_PKEY *pkey, const char *prefix, size_t prefixLength)
    NS_GNUC_NONNULL(1,2);

# ifndef HAVE_OPENSSL_3
static bool PkeyMatchesSubstring(EVP_PKEY *pkey, const char *needle)
    NS_GNUC_NONNULL(1,2);
# endif

static bool CryptoKeyTypeSupported(const char *name)
    NS_GNUC_NONNULL(1);

static const char *SignatureDefaultKeyName(void);

static bool PkeySignatureRequiresDigest(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static bool PkeySignatureSupportsNullDigest(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static int PkeySignatureDigestGet(Tcl_Interp *interp, EVP_PKEY *pkey,
                                 const char *digestName,
                                 const EVP_MD **mdPtr)
    NS_GNUC_NONNULL(1,2,4);

static const char *PkeySignatureDigestDefaultName(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static bool PkeySignatureAcceptsId(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static int PkeySignatureAcceptsIdFromObj(Tcl_Interp *interp, EVP_PKEY *pkey, Tcl_Obj *idObj,
                              const unsigned char **idPtr, size_t *idLengthPtr)
    NS_GNUC_NONNULL(1,2,4,5);

static int PkeySignatureSign(Tcl_Interp *interp, EVP_PKEY *pkey,
                             const unsigned char *message, size_t messageLength,
                             const unsigned char *id, size_t idLength,
                             const EVP_MD *md, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2,3);

static int PkeySignatureVerify(Tcl_Interp *interp, EVP_PKEY *pkey,
                               const unsigned char *message, size_t messageLength,
                               const unsigned char *signature, size_t signatureLength,
                               const unsigned char* id, size_t idLength,
                               const EVP_MD *md)
    NS_GNUC_NONNULL(1,2,3,5);

static int PkeyInfoPutBnPad(Tcl_Interp *interp, Tcl_Obj *resultObj,
                            const char *name, const BIGNUM *bn, size_t width,
                            Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2,3,4);

static int PkeyInfoPutLegacyDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1,2,3);

static int PkeyInfoPutCapabilities(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey)
        NS_GNUC_NONNULL(1,2,3);

static int PkeyInfoPutEcDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey, Ns_BinaryEncoding  encoding)
    NS_GNUC_NONNULL(1,2,3);

static int PkeyInfoPutOkpDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2,3);

static int PkeyInfoPutRsaDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey, Ns_BinaryEncoding  encoding)
    NS_GNUC_NONNULL(1,2,3);


# ifndef OPENSSL_NO_EC
static int CurveNidGet(Tcl_Interp *interp, const char *curveName, int *nidPtr)
    NS_GNUC_NONNULL(1,2,3);

static void SetResultFromEC_POINT(Tcl_Interp *interp, Tcl_DString *dsPtr, EC_KEY *eckey, const EC_POINT *ecpoint,
                                  BN_CTX *bn_ctx, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2,3,4,5);

static int EcGroupCoordinateLength(const char *groupName, size_t *coordLenPtr)
    NS_GNUC_NONNULL(1,2);
# endif /* OPENSSL_NO_EC */

static int GetCipher(Tcl_Interp *interp, const char *cipherName, unsigned long flags,
                     const char *modeMsg, const EVP_CIPHER **cipherPtr)
    NS_GNUC_NONNULL(1,2,4,5);

static bool AEAD_Set_ivlen(EVP_CIPHER_CTX *ctx, size_t ivlen)
    NS_GNUC_NONNULL(1);
static bool AEAD_Set_tag(EVP_CIPHER_CTX *ctx, const unsigned char *tag, size_t taglen)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static bool AEAD_Get_tag(EVP_CIPHER_CTX *ctx, unsigned char *tag, size_t taglen)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


static char *uuid_format(unsigned char *b, char *dst) NS_GNUC_NONNULL(1,2) NS_GNUC_PURE;
static const char *uuid_v4(char *dst) NS_GNUC_NONNULL(1);
static const char *uuid_v7(char *dst) NS_GNUC_NONNULL(1);

static TCL_OBJCMDPROC_T CryptoHmacAddObjCmd;
static TCL_OBJCMDPROC_T CryptoHmacFreeObjCmd;
static TCL_OBJCMDPROC_T CryptoHmacGetObjCmd;
static TCL_OBJCMDPROC_T CryptoHmacNewObjCmd;
static TCL_OBJCMDPROC_T CryptoHmacStringObjCmd;

static TCL_OBJCMDPROC_T CryptoMdAddObjCmd;
static TCL_OBJCMDPROC_T CryptoMdFreeObjCmd;
static TCL_OBJCMDPROC_T CryptoMdGetObjCmd;
static TCL_OBJCMDPROC_T CryptoMdNewObjCmd;
static TCL_OBJCMDPROC_T CryptoMdStringObjCmd;

static TCL_OBJCMDPROC_T CryptoKeyInfoObjCmd;
static TCL_OBJCMDPROC_T CryptoKeyPrivObjCmd;
static TCL_OBJCMDPROC_T CryptoKeyPubObjCmd;
static TCL_OBJCMDPROC_T CryptoKeyTypeObjCmd;

static TCL_OBJCMDPROC_T CryptoPkeySignatureSignObjCmd;
static TCL_OBJCMDPROC_T CryptoPkeySignatureVerifyObjCmd;

# ifndef OPENSSL_NO_EC
static TCL_OBJCMDPROC_T CryptoEckeyFromCoordsObjCmd;
static TCL_OBJCMDPROC_T CryptoEckeyGenerateObjCmd;
static TCL_OBJCMDPROC_T CryptoEckeyPubObjCmd;
#  ifndef HAVE_OPENSSL_PRE_1_1
static TCL_OBJCMDPROC_T CryptoEckeySharedsecretObjCmd;
#  endif
#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
static TCL_OBJCMDPROC_T CryptoEckeyPrivObjCmd;
static TCL_OBJCMDPROC_T CryptoEckeyImportObjCmd;
#  endif

static EVP_PKEY *PkeyGetFromEcKey(Tcl_Interp *interp, EC_KEY *eckey)
    NS_GNUC_NONNULL(1,2);
# endif

# ifdef HAVE_OPENSSL_3
static TCL_OBJCMDPROC_T CryptoAgreementGenerateObjCmd;
static TCL_OBJCMDPROC_T CryptoAgreementPubObjCmd;
static TCL_OBJCMDPROC_T CryptoAgreementDeriveObjCmd;
static TCL_OBJCMDPROC_T CryptoKeyGenerateObjCmd;
static TCL_OBJCMDPROC_T CryptoKeyImportObjCmd;

static TCL_OBJCMDPROC_T CryptoSignatureGenerateObjCmd;
static TCL_OBJCMDPROC_T CryptoSignaturePubObjCmd;

static int FreelistAdd(Tcl_Interp *interp, Ns_DList *dlPtr, void *ptr, Ns_FreeProc freeProc)
    NS_GNUC_NONNULL(1,2,3,4);

static void FreelistFree(void *arg)
    NS_GNUC_NONNULL(1);

static int GeneratePrivateKeyPem(Tcl_Interp *interp, const char *typeName, const char *what,
                                 const char *outfileName, NsCryptoKeygenUsage usage, OSSL_PARAM *params)
    NS_GNUC_NONNULL(1,2,3,4);

static int KeygenGroupParams(Tcl_Interp *interp, const char *typeName, const char *groupName, const char *what,
                             OSSL_PARAM params[2], OSSL_PARAM **paramPtr)
    NS_GNUC_NONNULL(1,2,4,5);

static void KeymgmtListCallback(EVP_KEYMGMT *keymgmt, void *arg)
    NS_GNUC_NONNULL(1,2);

static unsigned PkeyInstanceCapabilities(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static unsigned PkeyProbeSignatureCaps(EVP_PKEY *pkey)
    NS_GNUC_NONNULL(1);

static int PkeySignatureInitSm2(Tcl_Interp *interp, EVP_MD_CTX *mdctx,  EVP_PKEY *pkey,
                                const EVP_MD *md, const unsigned char *id, size_t idLength,
                                bool sign, EVP_PKEY_CTX **pctxPtr)
    NS_GNUC_NONNULL(1,2,3,8);

static int PkeyInfoPutProviderDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey)
        NS_GNUC_NONNULL(1,2,3);

static int PkeyImportParamsFromDict(Tcl_Interp *interp, const char *typeName, Ns_CryptoKeyImportSelection selection,
                                    Tcl_Obj *paramsObj, const char **resolvedTypeNamePtr,
                                    OSSL_PARAM_BLD *bld, Ns_DList *tmpData)
    NS_GNUC_NONNULL(1,2,4,5,6,7);

#  ifndef OPENSSL_NO_EC
static int PkeyImportEcFromCoords(Tcl_Interp *interp, const char *groupName, const unsigned char *x, size_t xLen,
                                  const unsigned char *y, size_t yLen, int outputFormat, const char *outfileName)
    NS_GNUC_NONNULL(1,2,3,5);

static int PkeyImportEcPublicParamsFromDict(Tcl_Interp *interp, Tcl_Obj *paramsObj, OSSL_PARAM_BLD *bld, Ns_DList *tmpData)
    NS_GNUC_NONNULL(1,2,3,4);
#   endif /* OPENSSL_NO_EC */

static int PkeyImportRsaPublicParamsFromDict(Tcl_Interp *interp, Tcl_Obj *paramsObj, OSSL_PARAM_BLD *bld,
                                  Ns_DList *tmpData)
    NS_GNUC_NONNULL(1,2,3,4);

static int PkeyImportFromParams(Tcl_Interp *interp, const char *typeName, Ns_CryptoKeyImportSelection selection,
                                OSSL_PARAM *params, int formatInt, Ns_BinaryEncoding encoding, const char *outfileName)
    NS_GNUC_NONNULL(1,2,4);

static int OkpCurveInfo(const char *crv, const char **typeNamePtr, size_t *pubLenPtr)
    NS_GNUC_NONNULL(1,2,3);

static int PkeyImportOkpPublicParamsFromDict(Tcl_Interp *interp, Tcl_Obj *paramsObj,
                                  const char **resolvedTypeNamePtr, OSSL_PARAM_BLD *bld,
                                  Ns_DList *tmpData)
    NS_GNUC_NONNULL(1,2,3,4,5);

static int PkeyInfoPutOctets(Tcl_Interp *interp, Tcl_Obj *resultObj, const char *name,
                             const unsigned char *value, size_t valueLen, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2,3,4);

static int
PkeyInfoPutOctetParam(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey, const char *dictName,
                      const char *paramName, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2,3,4,5);

# endif /* HAVE_OPENSSL_3 */

# ifdef HAVE_OPENSSL_3_5
static TCL_OBJCMDPROC_T CryptoKemGenerateObjCmd;
static TCL_OBJCMDPROC_T CryptoKemPubObjCmd;
static TCL_OBJCMDPROC_T CryptoKemEncapsulateObjCmd;
static TCL_OBJCMDPROC_T CryptoKemDecapsulateObjCmd;

static int PkeyKemEncapsulate(Tcl_Interp *interp, EVP_PKEY *pkey, Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1,2);
# endif /* HAVE_OPENSSL_3_5 */

/*
 * Local variables defined in this file.
 */

static const char * const mdCtxType  = "ns:mdctx";
static const char * const hmacCtxType  = "ns:hmacctx";

# ifdef HAVE_OPENSSL_HKDF
static Ns_ObjvValueRange posIntRange0 = {0, INT_MAX};
# endif

static Ns_ObjvValueRange posIntRange1 = {1, INT_MAX};


/*
 *----------------------------------------------------------------------
 *
 * SetResultFromOsslError --
 *
 *      Set the interpreter result from the current OpenSSL error queue,
 *      optionally prefixed with a caller-provided message.
 *
 * Results:
 *      None. The interpreter result is set.
 *
 * Side effects:
 *      Consumes the OpenSSL per-thread error queue and overwrites the
 *      interpreter result.
 *
 * Description:
 *      Retrieves the most relevant OpenSSL error, formats it into a
 *      human-readable message, and prepends the optional prefix.
 *      Should be called immediately after a failing OpenSSL call.
 *
 *----------------------------------------------------------------------
 */
static void
SetResultFromOsslError(Tcl_Interp *interp, const char *prefix)
{
    unsigned long err = ERR_peek_last_error();

    if (err == 0ul) {
        Ns_TclPrintfResult(interp, "%s", prefix);
    } else {
        const char *reason = ERR_reason_error_string(err);

        if (reason == NULL) {
            Ns_TclPrintfResult(interp, "%s", prefix);
        } else {
            Ns_TclPrintfResult(interp, "%s: %s", prefix, reason);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Debug function to ease work with binary data.
 *
 *----------------------------------------------------------------------
 */
static void hexPrint(const char *msg, const unsigned char *octets, size_t octetLength)
{
    if (Ns_LogSeverityEnabled(Debug)) {
        size_t i;
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        Ns_DStringPrintf(&ds, "%s (len %" PRIuz "): ", msg, octetLength);
        for (i = 0; i < octetLength; i++) {
            Ns_DStringPrintf(&ds, "%.2x ", octets[i] & 0xff);
        }
        Ns_Log(Debug, "%s", ds.string);
        Tcl_DStringFree(&ds);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Compatibility functions for older versions of OpenSSL.
 *
 *----------------------------------------------------------------------
 */

# ifdef HAVE_OPENSSL_PRE_1_1
#  define NS_EVP_MD_CTX_new  EVP_MD_CTX_create
#  define NS_EVP_MD_CTX_free EVP_MD_CTX_destroy

static HMAC_CTX *HMAC_CTX_new(void);
static void HMAC_CTX_free(HMAC_CTX *ctx) NS_GNUC_NONNULL(1);

# else
#  define NS_EVP_MD_CTX_new  EVP_MD_CTX_new
#  define NS_EVP_MD_CTX_free EVP_MD_CTX_free
# endif

# ifdef HAVE_OPENSSL_PRE_1_1
/*
 *----------------------------------------------------------------------
 *
 * HMAC_CTX_new, HMAC_CTX_free --
 *
 *      The NEW/FREE interface for HMAC_CTX is new in OpenSSL 1.1.0.
 *      Before, HMAC_CTX_init and HMAC_CTX_cleanup were used. We
 *      provide here a forward compatible version.
 *
 *----------------------------------------------------------------------
 */
static HMAC_CTX *HMAC_CTX_new(void)
{
    HMAC_CTX *ctx = ns_malloc(sizeof(HMAC_CTX));
    HMAC_CTX_init(ctx);
    return ctx;
}

static void HMAC_CTX_free(HMAC_CTX *ctx)
{
    NS_NONNULL_ASSERT(ctx != NULL);

    HMAC_CTX_cleanup(ctx);
    ns_free(ctx);
}
# endif


# ifdef HAVE_OPENSSL_PRE_1_1
#  ifndef OPENSSL_NO_EC
/*
 *----------------------------------------------------------------------
 *
 * ECDSA_SIG_get0 --
 *
 *      The function ECDSA_SIG_get0 is new in OpenSSL 1.1.0 (and not
 *      available in LIBRESSL). We provide here a forward compatible
 *      version.
 *
 *----------------------------------------------------------------------
 */
static void ECDSA_SIG_get0(const ECDSA_SIG *sig, const BIGNUM **pr, const BIGNUM **ps)
{
    if (pr != NULL) {
        *pr = sig->r;
    }
    if (ps != NULL) {
        *ps = sig->s;
    }
}
#  endif
# endif /* HAVE_OPENSSL_PRE_1_1 */

# ifdef HAVE_OPENSSL_3
/*
 * OpenSSL 3.x: use parameter-based API
 */

static bool AEAD_Set_ivlen(EVP_CIPHER_CTX *ctx, size_t ivlen) {
    OSSL_PARAM params[2];

    params[0] = OSSL_PARAM_construct_size_t(
                    OSSL_CIPHER_PARAM_IVLEN, &ivlen);
    params[1] = OSSL_PARAM_construct_end();
    return EVP_CIPHER_CTX_set_params(ctx, params) > 0;
}
static bool AEAD_Set_tag(EVP_CIPHER_CTX *ctx,
                        const unsigned char *tag, size_t taglen) {
    OSSL_PARAM params[2];

    params[0] = OSSL_PARAM_construct_octet_string(
                    OSSL_CIPHER_PARAM_AEAD_TAG,
                    ns_const2voidp(tag), taglen);
    params[1] = OSSL_PARAM_construct_end();
    return EVP_CIPHER_CTX_set_params(ctx, params) > 0;
}
static bool AEAD_Get_tag(EVP_CIPHER_CTX *ctx,
                        unsigned char *tag, size_t taglen) {
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_octet_string(
                    OSSL_CIPHER_PARAM_AEAD_TAG,
                    tag, taglen);
    params[1] = OSSL_PARAM_construct_end();
    return EVP_CIPHER_CTX_get_params(ctx, params) > 0;
}

# else
/*
 * OpenSSL 1.x: use legacy API
 */
static bool AEAD_Set_ivlen(EVP_CIPHER_CTX *ctx, size_t ivlen) {
    return EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)ivlen, NULL);
}
static bool AEAD_Set_tag(EVP_CIPHER_CTX *ctx,
                        const unsigned char *tag, size_t taglen) {
    return EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                               (int)taglen, (void *)tag);
}
static bool AEAD_Get_tag(EVP_CIPHER_CTX *ctx,
                        unsigned char *tag, size_t taglen) {
    return EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                               (int)taglen, tag);
}
static int PkeyEcFromCoordsLegacy(Tcl_Interp *interp,
                                  const char *curveName,
                                  const unsigned char *xBytes, size_t xLen,
                                  const unsigned char *yBytes, size_t yLen,
                                  int formatInt,
                                  const char *outfileName)
    NS_GNUC_NONNULL(1,2,3,8);
# endif /* HAVE_OPENSSL_3 */

/*
 *----------------------------------------------------------------------
 *
 * SetResultFromMemBio --
 *
 *      Return the contents of a memory BIO as Tcl result.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
SetResultFromMemBio(Tcl_Interp *interp, BIO *bio, const char *what)
{
    BUF_MEM *bptr = NULL;

    if (BIO_get_mem_ptr(bio, &bptr) != 1
        || bptr == NULL
        || bptr->data == NULL) {
        Ns_TclPrintfResult(interp, "could not obtain generated %s key", what);
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, Tcl_NewStringObj(bptr->data, (TCL_SIZE_T)bptr->length));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PemOpenWriteStream --
 *
 *      Create a BIO suitable for writing PEM output either to a file
 *      or to an in-memory buffer.
 *
 *      When an output file name is provided, a file BIO is opened for
 *      writing. Otherwise, a memory BIO is created, allowing the caller
 *      to retrieve the generated PEM data and return it as a Tcl result.
 *
 * Results:
 *      Returns a BIO pointer on success.
 *      Returns NULL on error and sets an error message in the Tcl
 *      interpreter result.
 *
 * Side effects:
 *      Opens a file for writing when outfileName is not NULL.
 *
 *----------------------------------------------------------------------
 */
static BIO *
PemOpenWriteStream(Tcl_Interp *interp, const char *outfileName)
{
    BIO *bio;

    bio = (outfileName != NULL)
        ? BIO_new_file(outfileName, "w")
        : BIO_new(BIO_s_mem());

    if (bio == NULL) {
        if (outfileName != NULL) {
            Ns_TclPrintfResult(interp, "could not open pem-file '%s' for writing", outfileName);
        } else {
            Ns_TclPrintfResult(interp, "could not allocate memory bio");
        }
    }

    return bio;
}

/*
 *----------------------------------------------------------------------
 *
 * PemWriteResult --
 *
 *      Finalize writing of PEM data produced via a BIO and propagate
 *      the result to the Tcl interpreter.
 *
 *      When an output file name is provided, the function assumes the
 *      BIO has already written the data to the file and returns TCL_OK.
 *      Otherwise, the content of the memory BIO is extracted and set
 *      as the Tcl result.
 *
 * Results:
 *      Returns TCL_OK on success.
 *      Returns TCL_ERROR if extracting the result from the memory BIO
 *      fails (via SetResultFromMemBio).
 *
 * Side effects:
 *      Sets the Tcl interpreter result when no output file is specified.
 *
 *----------------------------------------------------------------------
 */
#if defined(__clang__) && defined(__APPLE__)
/*
 * Work around an optimizer/code generation issue observed with
 * Apple clang (Xcode 21, arm64) when compiling with -Os:
 *
 * When this function is inlined, the compiler may miscompile the
 * NULL check on outfileName, causing the wrong branch to be taken
 * (SetResultFromMemBio() not called even when outfileName == NULL).
 *
 * The issue disappears when:
 *   - the function is not inlined (noinline), or
 *   - additional code (e.g., logging) inhibits the optimization.
 *
 * GCC (e.g., gcc-mp-15) does not exhibit this behavior.
 *
 * Keep this function out-of-line to ensure correct semantics.
 */
__attribute__((noinline))
#endif
static int
PemWriteResult(Tcl_Interp *interp, BIO *bio, const char *outfileName, const char *what)
{
    if (outfileName == NULL) {
        return SetResultFromMemBio(interp, bio, what);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyPublicWrite --
 *
 *      Serialize an EVP_PKEY public key into PEM or DER format and
 *      either return the result to the Tcl interpreter or write it
 *      to a file.
 *
 *      When wantPem is true, the public key is written as a PEM encoded
 *      SubjectPublicKeyInfo ("BEGIN PUBLIC KEY") using a BIO obtained
 *      via PemOpenWriteStream().
 *
 *      When wantPem is false, the public key is encoded as DER using
 *      i2d_PUBKEY(). If an output file is specified, the DER data is
 *      written directly to the file. Otherwise, the DER data is returned
 *      as binary Tcl data.
 *
 * Results:
 *      Returns TCL_OK on success.
 *      Returns TCL_ERROR on failure and sets an error message in the
 *      Tcl interpreter result.
 *
 * Side effects:
 *      May open and write to a file when outfileName is not NULL.
 *      Sets the Tcl interpreter result when no output file is specified.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyPublicWrite(Tcl_Interp *interp, EVP_PKEY *pkey,
               const char *outfileName, bool wantPem)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(pkey != NULL);

    if (wantPem) {
        BIO *bio = PemOpenWriteStream(interp, outfileName);
        int result;

        if (bio == NULL) {
            return TCL_ERROR;
        }

        if (PEM_write_bio_PUBKEY(bio, pkey) != 1) {
            Ns_TclPrintfResult(interp, "could not write public key");
            BIO_free(bio);
            return TCL_ERROR;
        }

        result = PemWriteResult(interp, bio, outfileName, "public");
        BIO_free(bio);

        return result;

    } else {
        int len = i2d_PUBKEY(pkey, NULL);
        unsigned char *buf, *p;

        if (len <= 0) {
            Ns_TclPrintfResult(interp, "i2d_PUBKEY failed");
            return TCL_ERROR;
        }

        buf = ns_malloc((size_t)len);
        p   = buf;

        if (i2d_PUBKEY(pkey, &p) != len) {
            ns_free(buf);
            Ns_TclPrintfResult(interp, "i2d_PUBKEY produced unexpected length");
            return TCL_ERROR;
        }

        if (outfileName != NULL) {
            FILE *f = fopen(outfileName, "wb");
            if (f == NULL) {
                Ns_TclPrintfResult(interp, "could not open file '%s' for writing", outfileName);
                ns_free(buf);
                return TCL_ERROR;
            }
            if (fwrite(buf, 1u, (size_t)len, f) != (size_t)len) {
                Ns_TclPrintfResult(interp, "could not write DER key to file '%s'", outfileName);
                fclose(f);
                ns_free(buf);
                return TCL_ERROR;
            }
            fclose(f);
            ns_free(buf);
            return TCL_OK;

        } else {
            Tcl_SetObjResult(interp,
                NsEncodedObj(buf, (size_t)len, NULL, NS_OBJ_ENCODING_BINARY));
            ns_free(buf);
            return TCL_OK;
        }
    }
}

# ifdef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * FreelistAdd --
 *
 *      Add a temporary object together with its free procedure to a
 *      cleanup list. The ownership is always transferred to this function.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on allocation failure.
 *
 * Side effects:
 *      Allocates a bookkeeping entry and appends it to the provided
 *      Ns_DList. The object will be released later via FreelistFree().
 *
 *----------------------------------------------------------------------
 */
static int
FreelistAdd(Tcl_Interp *interp, Ns_DList *dlPtr, void *ptr, Ns_FreeProc freeProc)
{
    Ns_OsslTmp *entry;

    entry = ns_malloc(sizeof(Ns_OsslTmp));
    if (entry == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate temporary cleanup record");
        (*freeProc)(ptr);
        return TCL_ERROR;
    }

    entry->ptr = ptr;
    entry->freeProc = freeProc;
    Ns_DListAppend(dlPtr, entry);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FreelistFree --
 *
 *      Free a single entry from the temporary cleanup list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Invokes the stored free procedure on the associated pointer and
 *      releases the bookkeeping structure itself.
 *
 *----------------------------------------------------------------------
 */
static void
FreelistFree(void *arg)
{
    Ns_OsslTmp *entry = arg;

    if (entry != NULL) {
        if (entry->ptr != NULL && entry->freeProc != NULL) {
            (*entry->freeProc)(entry->ptr);
        }
        ns_free(entry);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * KeygenGroupParams --
 *
 *      Validate the optional -group parameter for provider-based key
 *      generation and, when applicable, prepare OpenSSL parameter
 *      settings.
 *
 *      For EC, DH, and DHX, -group is required. For X25519 and X448,
 *      -group is optional but, when specified, must match the fixed
 *      algorithm name. For other key types, -group is not supported.
 *
 * Parameters:
 *      interp     - Tcl interpreter for error reporting
 *      typeName   - key algorithm name
 *      groupName  - optional group name, may be empty
 *      what       - wording for error messages (e.g. "key agreement",
 *                   "key")
 *      params     - caller-provided two-element OSSL_PARAM array
 *      paramPtr   - result pointer receiving NULL or params
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      On success, *paramPtr is set either to NULL or to params.
 *      On failure, an error message is left in the interpreter.
 *
 *----------------------------------------------------------------------
 */
static int
KeygenGroupParams(Tcl_Interp *interp,
                  const char *typeName,
                  const char *groupName,
                  const char *what,
                  OSSL_PARAM params[2],
                  OSSL_PARAM **paramPtr)
{
    bool requiresGroup = NS_FALSE;
    bool acceptsOptionalGroup = NS_FALSE;

    *paramPtr = NULL;

    if (groupName != NULL && *groupName == '\0') {
        groupName = NULL;
    }

    /*
     * Provide a default named group for EC for consistency with the
     * legacy eckey interface.
     */
    if (STRIEQ(typeName, "EC")) {
        requiresGroup = NS_TRUE;
        if (groupName == NULL) {
            groupName = "prime256v1";
        }

    } else if (STRIEQ(typeName, "DH")
               || STRIEQ(typeName, "DHX")
               ) {
        requiresGroup = NS_TRUE;

    } else if (STRIEQ(typeName, "X25519")
               || STRIEQ(typeName, "X448")
               ) {
        acceptsOptionalGroup = NS_TRUE;
    }

    if (requiresGroup) {
        if (groupName == NULL) {
            Ns_TclPrintfResult(interp,
                               "the option \"-group\" is required for %s algorithm \"%s\"",
                               what, typeName);
            return TCL_ERROR;
        }

    } else if (acceptsOptionalGroup) {
        if (groupName != NULL) {
            if ((STRIEQ(typeName, "X25519")  && !STRIEQ(groupName, "x25519"))
                || (STRIEQ(typeName, "X448") && !STRIEQ(groupName, "x448"))) {
                Ns_TclPrintfResult(interp,
                                   "group \"%s\" is not valid for %s algorithm \"%s\"",
                                   groupName, what, typeName);
                return TCL_ERROR;
            }
        }

    } else if (groupName != NULL) {
        Ns_TclPrintfResult(interp,
                           "the option \"-group\" is not supported for %s algorithm \"%s\"",
                           what, typeName);
        return TCL_ERROR;
    }

    if (groupName != NULL) {
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                     (char *)groupName, 0);
        params[1] = OSSL_PARAM_construct_end();
        *paramPtr = params;
    }

    return TCL_OK;
}

/*----------------------------------------------------------------------
 *
 * KeymgmtListCallback --
 *
 *      Callback used with EVP_KEYMGMT_do_all_provided() to collect
 *      available key management algorithm names.
 *
 *      The function filters algorithms based on the requested usage
 *      stored in NsKeygenListCtx:
 *
 *        - NS_CRYPTO_KEYGEN_USAGE_SIGNATURE:
 *            include signature-capable algorithms (e.g., RSA,
 *            RSA-PSS, ED25519, ED448, ML-DSA-*, SLH-DSA-*)
 *
 *        - NS_CRYPTO_KEYGEN_USAGE_KEM:
 *            include key encapsulation mechanisms (ML-KEM-*)
 *
 *        - NS_CRYPTO_KEYGEN_USAGE_ANY:
 *            include all available algorithms
 *
 *      Matching algorithm names are appended to the Tcl list provided
 *      in the context structure.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Appends zero or more elements to ctx->listObj.
 *
 *----------------------------------------------------------------------
 */
static void
KeymgmtListCallback(EVP_KEYMGMT *keymgmt, void *arg)
{
    NsKeygenListCtx *ctx = arg;
    bool                addToList;
    const char         *name = EVP_KEYMGMT_get0_name(keymgmt);

    if (name == NULL) {
        return;
    }

    if (ctx->usage == NS_CRYPTO_KEYGEN_USAGE_SIGNATURE) {
        addToList = (STREQ(name, "RSA")
                     || STREQ(name, "RSA-PSS")
                     || STREQ(name, "EC")
                     || STREQ(name, "ED25519")
                     || STREQ(name, "ED448")
                     || strncmp(name, "ML-DSA-", 7) == 0
                     || strncmp(name, "SLH-DSA-", 8) == 0
                     );
    } else if (ctx->usage == NS_CRYPTO_KEYGEN_USAGE_KEM) {
        addToList = (strncmp(name, "ML-KEM-", 7) == 0
                     || STREQ(name, "EC")
                     //|| strstr(name, "MLKEM") != NULL
                     );
    } else if (ctx->usage == NS_CRYPTO_KEYGEN_USAGE_AGREEMENT) {
        addToList = (NS_FALSE
                     || STREQ(name, "DH")
                     || STREQ(name, "DHX")
                     || STREQ(name, "EC")
                     //|| STREQ(name, "SM2")
                     || STREQ(name, "X25519")
                     || STREQ(name, "X448")
                     );
    } else /* ctx->usage == NS_CRYPTO_KEYGEN_USAGE_ANY */ {
        addToList = NS_TRUE;
    }
    Ns_Log(Debug, "key type name %s add to list %d", name, addToList);

    if (addToList) {
        Tcl_ListObjAppendElement(NULL, ctx->listObj,
                                 Tcl_NewStringObj(name, TCL_INDEX_NONE));
    }
}


/*----------------------------------------------------------------------
 *
 * GeneratePrivateKeyPem --
 *
 *      Generate a private key of the specified type via OpenSSL's
 *      provider-based key management API and return it as PEM or
 *      write it to a file.
 *
 *      The key type is specified via "typeName" and resolved through
 *      EVP_PKEY_CTX_new_from_name(). The function performs the
 *      following steps:
 *
 *        - create a key generation context
 *        - initialize key generation
 *        - generate the key
 *        - validate usage constraints (e.g., signature or KEM)
 *        - write the key in PEM format
 *
 *      The "usage" parameter restricts the allowed key types:
 *        - NS_CRYPTO_KEYGEN_USAGE_SIGNATURE: key must support signing
 *        - NS_CRYPTO_KEYGEN_USAGE_KEM:       key must support KEM
 *
 *      The "what" argument is used for consistent error reporting
 *      (e.g., "signature", "key encapsulation").
 *
 *      When the algorithm name is unknown or unsupported, a list of
 *      valid names (filtered by usage) is generated via
 *      EVP_KEYMGMT_do_all_provided() and returned in the error message.
 *
 * Results:
 *      TCL_OK    - key successfully generated and returned/written
 *      TCL_ERROR - on failure (error message set in interpreter)
 *
 * Side effects:
 *      Allocates OpenSSL EVP_PKEY and BIO objects.
 *      May create or overwrite a file when outfileName is provided.
 *      Sets the Tcl interpreter result when no output file is used.
 *
 *----------------------------------------------------------------------
 */
static int
GeneratePrivateKeyPem(Tcl_Interp *interp,
                      const char *typeName,
                      const char *what,
                      const char *outfileName,
                      NsCryptoKeygenUsage usage,
                      OSSL_PARAM *params)
{
    int           result = TCL_ERROR;
    EVP_PKEY_CTX *ctx  = NULL;
    EVP_PKEY     *pkey = NULL;
    BIO          *bio  = NULL;

    if (*typeName == '\0') {
        Ns_TclPrintfResult(interp, "missing %s algorithm name", what);
        goto done;
    }

    ctx = EVP_PKEY_CTX_new_from_name(NULL, typeName, NULL);
    if (ctx == NULL) {
        NsKeygenListCtx sigCtx;
        const char         *sortedNames;
        Tcl_Obj            *sorted;

        sigCtx.listObj = Tcl_NewListObj(0, NULL);
        sigCtx.usage = usage;
        Tcl_IncrRefCount(sigCtx.listObj);

        EVP_KEYMGMT_do_all_provided(NULL, KeymgmtListCallback, &sigCtx);
        sorted = NsTclListSort(interp, sigCtx.listObj);
        sortedNames = (sorted != NULL ? Tcl_GetString(sorted) : "");

        Ns_TclPrintfResult(interp,
                           "unknown or unsupported %s algorithm \"%s\","
                           " supported names: %s",
                           what, typeName, sortedNames);
        Tcl_DecrRefCount(sigCtx.listObj);
        goto done;
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        Ns_TclPrintfResult(interp,
                           "could not initialize key generation for %s \"%s\"",
                           what, typeName);
        goto done;
    }

    if (params != NULL && EVP_PKEY_CTX_set_params(ctx, params) <= 0) {
        Ns_TclPrintfResult(interp,
                           "could not set parameters for %s algorithm \"%s\"",
                           what, typeName);
        goto done;
    }

    if (EVP_PKEY_generate(ctx, &pkey) <= 0) {
        Ns_TclPrintfResult(interp,
                           "key type \"%s\" cannot be used for %s generation",
                           typeName, what);
        goto done;
    }

    if (usage == NS_CRYPTO_KEYGEN_USAGE_SIGNATURE && !PkeySupportsSignature(pkey)) {
        Ns_TclPrintfResult(interp,
                           "generated key \"%s\" does not support %s operations",
                           typeName, what);
        goto done;
    }

    if (usage == NS_CRYPTO_KEYGEN_USAGE_KEM && !PkeySupportsKem(pkey)) {
        Ns_TclPrintfResult(interp,
                           "generated key \"%s\" does not support %s operations",
                           typeName, what);
        goto done;
    }

    if (usage == NS_CRYPTO_KEYGEN_USAGE_AGREEMENT && !PkeySupportsAgreement(pkey)) {
        Ns_TclPrintfResult(interp,
                           "generated key \"%s\" does not support %s operations",
                           typeName, what);
        goto done;
    }


    bio = PemOpenWriteStream(interp, outfileName);
    if (bio == NULL) {
        goto done;
    }

    if (PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
        Ns_TclPrintfResult(interp,
                           "could not write %s key \"%s\"",
                           what, typeName);
        goto done;
    }

    result = PemWriteResult(interp, bio, outfileName, what);

done:
    if (bio != NULL) {
        BIO_free(bio);
    }
    if (pkey != NULL) {
        EVP_PKEY_free(pkey);
    }
    if (ctx != NULL) {
        EVP_PKEY_CTX_free(ctx);
    }

    return result;
}
# endif /* HAVE_OPENSSL_3 */

/*----------------------------------------------------------------------
 *
 * PkeyPublicPemWrite --
 *
 *      Serialize the public key of the provided EVP_PKEY into PEM
 *      format and either return it as Tcl result or write it to a
 *      file.
 *
 *      The function creates a BIO via PemOpenWriteStream(), writes
 *      the public key using PEM_write_bio_PUBKEY(), and finalizes the
 *      result via PemWriteResult().
 *
 *      The "what" argument is used to construct human-readable error
 *      messages (e.g., "signature", "key encapsulation").
 *
 * Results:
 *      TCL_OK    - public key successfully written or returned
 *      TCL_ERROR - on failure (error message set in interpreter)
 *
 * Side effects:
 *      May create or overwrite a file when outfileName is provided.
 *      Sets the Tcl interpreter result when no output file is used.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyPublicPemWrite(Tcl_Interp *interp,
                  EVP_PKEY   *pkey,
                  const char *what,
                  const char *resultWhat,
                  const char *outfileName)
{
    BIO *bio = NULL;
    int  result;

    bio = PemOpenWriteStream(interp, outfileName);
    if (bio == NULL) {
        return TCL_ERROR;
    }

    if (PEM_write_bio_PUBKEY(bio, pkey) != 1) {
        Ns_TclPrintfResult(interp,
                           "could not write %s public key",
                           what);
        BIO_free(bio);
        return TCL_ERROR;
    }

    result = PemWriteResult(interp, bio, outfileName, resultWhat);

    BIO_free(bio);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DigestGet, DigestListCallback --
 *
 *      Converter from a digest string to internal OpenSSL
 *      representation.  DigestListCallback is an iterator usable in OpenSSL
 *      1.0.0 or newer to obtain the names of all available digest
 *      functions to provide nicer error messages.
 *
 * Results:
 *      Tcl result code, value in third argument.
 *
 * Side effects:
 *      Interp result Obj is updated.
 *
 *----------------------------------------------------------------------
 */

# if !defined(HAVE_OPENSSL_PRE_1_0) && !defined(HAVE_OPENSSL_3)
static void
DigestListCallback(const EVP_MD *m, const char *from, const char *UNUSED(to), void *arg)
{
    Tcl_Obj *listPtr = (Tcl_Obj *)arg;

    if ((m != NULL) && (from != NULL)) {
        const char *mdName = EVP_MD_name(m);

        /* fprintf(stderr, "from %s to %to name <%s> type (nid) %d\n", from, to, mdName, EVP_MD_type(m)); */
        /*
         * Apparently, the list contains upper and lowercase variants. Avoid
         * duplication.
         */
        if ((*from >= 'a') && (*from <= 'z')) {
            (void)Tcl_ListObjAppendElement(NULL, listPtr, Tcl_NewStringObj(mdName, TCL_INDEX_NONE));
        }
    }
}
# endif

/*----------------------------------------------------------------------
 *
 * DigestFree --
 *
 *      Release resources associated with an NsDigest structure.
 *
 *      In OpenSSL 3.x builds, this function frees the fetched digest
 *      implementation obtained via EVP_MD_fetch(). For legacy builds,
 *      no action is required beyond resetting the pointer.
 *
 * Parameters:
 *      digestPtr - pointer to NsDigest structure to clean up
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May call EVP_MD_free() when using OpenSSL 3.x. The fields of
 *      digestPtr are reset to NULL.
 *
 *----------------------------------------------------------------------
 */
static void
DigestFree(NsDigest *digestPtr)
{
    NS_NONNULL_ASSERT(digestPtr != NULL);

# ifdef HAVE_OPENSSL_3
    if (digestPtr->fetchedMd != NULL) {
        EVP_MD_free(digestPtr->fetchedMd);
        digestPtr->fetchedMd = NULL;
    }
# endif
    digestPtr->md = NULL;
}

# ifdef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * DigestAllowed --
 *
 *      Decide whether a digest algorithm is suitable for the requested
 *      usage. This function filters out algorithms that are not usable
 *      with the current NaviServer crypto API (e.g., XOF-based digests
 *      requiring special handling).
 *
 *      In particular:
 *        - "NULL" is always rejected
 *        - XOF digests (SHAKE-*) are rejected (require explicit length)
 *        - KMAC digests (KECCAK-KMAC-*) are rejected (use EVP_MAC instead)
 *        - Plain KECCAK-* digests are allowed for message digests
 *        - HMAC excludes XOF/KMAC as well
 *
 * Parameters:
 *      name   - canonical OpenSSL digest name (must not be NULL)
 *      usage  - bitmask indicating intended usage (e.g., MD, HMAC, HKDF)
 *
 * Results:
 *      NS_TRUE if allowed, NS_FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
DigestAllowed(const char *name, NsDigestUsage usage)
{
    NS_NONNULL_ASSERT(name != NULL);

    /*
     * Reject "NULL" digest explicitly.
     */
    if (STREQ(name, "NULL")) {
        return NS_FALSE;
    }

    /*
     * Reject XOF digests (variable-length output, not supported by the
     * current EVP_DigestFinal_ex()-based code paths).
     */
    if (strncmp(name, "SHAKE-", 6) == 0) {
        return NS_FALSE;
    }

    /*
     * Reject KMAC digests. These are not plain message digests for our
     * current use cases and belong to EVP_MAC-oriented handling.
     */
    if (strncmp(name, "KECCAK-KMAC-", 12) == 0) {
        return NS_FALSE;
    }

    if ((usage & NS_DIGEST_USAGE_HMAC) != 0u) {
        /*
         * HMAC requires fixed-length digest functions.
         */
        return NS_TRUE;
    }

    if ((usage & NS_DIGEST_USAGE_HKDF) != 0u) {
        /*
         * HKDF requires a fixed-length digest function.
         */
        return NS_TRUE;
    }

    if ((usage & NS_DIGEST_USAGE_PBKDF2) != 0u) {
        /*
         * PBKDF2-HMAC requires a fixed-length digest function.
         */
        return NS_TRUE;
    }

    if ((usage & NS_DIGEST_USAGE_SIGN_VERIFY) != 0u) {
        /*
         * Signing/verifying via EVP_DigestSign/Verify currently accepts
         * the same digest set as plain message digests in NaviServer.
         */
        return NS_TRUE;
    }

    if ((usage & NS_DIGEST_USAGE_MD) != 0u) {
        /*
         * Plain message digests: allow standard fixed-length digests,
         * including KECCAK-*.
         */
        return NS_TRUE;
    }

    /*
     * Fallback: reject unknown/unspecified usage classes explicitly.
     */
    return NS_FALSE;
}

/*----------------------------------------------------------------------
 *
 * DigestListCallbackProvided --
 *
 *      Callback used with EVP_MD_do_all_provided() to collect available
 *      message digest algorithm names.
 *
 *      The function retrieves the canonical name of each provided digest
 *      and appends it to the Tcl list stored in the supplied context,
 *      subject to filtering via DigestAllowed().
 *
 * Parameters:
 *      md  - OpenSSL message digest implementation
 *      arg - pointer to NsDigestListCtx containing:
 *              listObj - Tcl list to append names to
 *              usage   - usage flags for filtering
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Appends zero or more elements to the Tcl list in the provided
 *      context. Elements are appended only when the digest name is
 *      non-NULL and allowed for the specified usage.
 *
 *----------------------------------------------------------------------
 */
static void
DigestListCallbackProvided(EVP_MD *md, void *arg)
{
    NsDigestListCtx *ctxPtr = arg;
    Tcl_Obj         *listObj;
    const char      *name;

    NS_NONNULL_ASSERT(md != NULL);
    NS_NONNULL_ASSERT(ctxPtr != NULL);

    listObj = ctxPtr->listObj;
    name = EVP_MD_get0_name(md);

    if (name != NULL && DigestAllowed(name, ctxPtr->usage)) {
        Tcl_ListObjAppendElement(NULL, listObj,
                                 Tcl_NewStringObj(name, TCL_INDEX_NONE));
    }
}

/*----------------------------------------------------------------------
 *
 * DigestGet --
 *
 *      Resolve a message digest by name and return an initialized
 *      NsDigest structure suitable for use with OpenSSL EVP APIs.
 *
 *      In OpenSSL 3.x builds, this function uses EVP_MD_fetch() to
 *      obtain a provider-based implementation and validates the
 *      resolved canonical name against the requested usage via
 *      DigestAllowed(). In legacy builds, EVP_get_digestbyname()
 *      is used without usage-based filtering.
 *
 *      When the lookup fails or the digest is not permitted for the
 *      specified usage, an error is returned and a Tcl error message
 *      is set, including a list of valid digest names (filtered and
 *      sorted when applicable).
 *
 * Parameters:
 *      interp      - Tcl interpreter for error reporting
 *      digestName  - name of the requested digest algorithm
 *      usage       - bitmask describing intended usage
 *                    (e.g., NS_DIGEST_USAGE_MD, _HMAC, _HKDF, ...)
 *      digestPtr   - pointer to NsDigest structure to be filled
 *
 * Results:
 *      TCL_OK    - digest successfully resolved and stored in digestPtr
 *      TCL_ERROR - digest not found or not allowed; error message set
 *
 * Side effects:
 *      On success, digestPtr is initialized and may contain a fetched
 *      EVP_MD object requiring later cleanup via DigestFree().
 *      On failure, a descriptive error message is stored in the
 *      interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
DigestGet(Tcl_Interp *interp, const char *digestName,
          NsDigestUsage usage, NsDigest *digestPtr)
{
    int         result;
    const char *resolvedName = NULL;

    digestPtr->md = NULL;
    digestPtr->fetchedMd = EVP_MD_fetch(NULL, digestName, NULL);
    if (digestPtr->fetchedMd != NULL) {
        digestPtr->md = digestPtr->fetchedMd;
        resolvedName = EVP_MD_get0_name(digestPtr->fetchedMd);

        if (resolvedName != NULL && DigestAllowed(resolvedName, usage)) {
            return TCL_OK;
        }

        DigestFree(digestPtr);
    }

    {
        Tcl_Obj         *listObj, *sortedObj;
        NsDigestListCtx  listCtx;

        listObj = Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(listObj);

        listCtx.listObj = listObj;
        listCtx.usage   = usage;

        EVP_MD_do_all_provided(NULL, DigestListCallbackProvided, &listCtx);

        sortedObj = NsTclListSort(interp, listObj);
        if (sortedObj != NULL) {
            Tcl_DecrRefCount(listObj);
            listObj = sortedObj;
            Tcl_IncrRefCount(listObj);
        }

        Ns_TclPrintfResult(interp, "Unknown value for digest \"%s\", valid: %s",
                           digestName, Tcl_GetString(listObj));
        Tcl_DecrRefCount(listObj);
    }

    result = TCL_ERROR;
    return result;
}

# else
/* legacy version */
static int
DigestGet(Tcl_Interp *interp, const char *digestName,
          NsDigestUsage UNUSED(usage), NsDigest *digestPtr)
{
    int result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(digestName != NULL);
    NS_NONNULL_ASSERT(digestPtr != NULL);

    digestPtr->md = EVP_get_digestbyname(digestName);
    if (digestPtr->md != NULL) {
        return TCL_OK;
    }

#  ifndef HAVE_OPENSSL_PRE_1_0
    {
        Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

        Tcl_IncrRefCount(listObj);
        EVP_MD_do_all_sorted(DigestListCallback, listObj);
        Ns_TclPrintfResult(interp, "Unknown value for digest \"%s\", valid: %s",
                           digestName, Tcl_GetString(listObj));
        Tcl_DecrRefCount(listObj);
    }
#  else
    Ns_TclPrintfResult(interp, "Unknown message digest \"%s\"", digestName);
#  endif

    result = TCL_ERROR;
    return result;
}
# endif


/*
 *----------------------------------------------------------------------
 *
 * GetCipher --
 *
 *      Helper function to lookup cipher from a string.
 *
 * Results:
 *      Tcl result code, value in third argument.
 *
 * Side effects:
 *      Interp result Obj is updated.
 *
 *----------------------------------------------------------------------
 */
static int
GetCipher(Tcl_Interp *interp, const char *cipherName, unsigned long flags, const char *modeMsg, const EVP_CIPHER **cipherPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cipherName != NULL);
    NS_NONNULL_ASSERT(modeMsg != NULL);
    NS_NONNULL_ASSERT(cipherPtr != NULL);

    *cipherPtr = EVP_get_cipherbyname(cipherName);
    if (*cipherPtr == NULL) {
        Ns_TclPrintfResult(interp, "Unknown cipher \"%s\"", cipherName);
        result = TCL_ERROR;
    } else if (flags != 0u) {
        int mode = EVP_CIPHER_mode(*cipherPtr);
        if (((unsigned)mode && flags) == 0u) {
            Ns_TclPrintfResult(interp, "cipher \"%s\" does not support require mode: %s", cipherName, modeMsg);
            result = TCL_ERROR;
        }
    }
    return result;
}

# ifndef OPENSSL_NO_EC
/*
 *----------------------------------------------------------------------
 *
 * CurveNidGet --
 *
 *      Helper function to lookup a nid from a curve name.
 *      The logic is from apps/ecparam.c
 *
 * Results:
 *      Tcl result code, value in third argument.
 *
 * Side effects:
 *      Interp result Obj is updated in case of error.
 *
 *----------------------------------------------------------------------
 */
static int
CurveNidGet(Tcl_Interp *interp, const char *curveName, int *nidPtr)
{
    int result, nid;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(curveName != NULL);
    NS_NONNULL_ASSERT(nidPtr != NULL);

    /*
     * Workaround for the SECG curve names secp192r1 and secp256r1 (which
     * are the same as the curves prime192v1 and prime256v1 defined in
     * X9.62).
     */
    if (STREQ(curveName, "secp192r1")) {
        Ns_Log(Warning, "using curve name prime192v1 instead of secp192r1");
        nid = NID_X9_62_prime192v1;
    } else if (STREQ(curveName, "secp256r1")) {
        Ns_Log(Warning, "using curve name prime256v1 instead of secp256r1");
        nid = NID_X9_62_prime256v1;
    } else {
        nid = OBJ_sn2nid(curveName);
    }
#  ifndef HAVE_OPENSSL_PRE_1_0_2
    if (nid == 0) {
        nid = EC_curve_nist2nid(curveName);
    }
#  endif
    if (nid == 0) {
        Ns_TclPrintfResult(interp, "Unknown curve name \"%s\"", curveName);
        result = TCL_ERROR;
    } else {
        *nidPtr = nid;
        result = TCL_OK;
    }
    return result;
}
# endif /* OPENSSL_NO_EC */

/*
 *----------------------------------------------------------------------
 *
 * PemOpenReadStream --
 *
 *      Open an OpenSSL BIO stream based on either the provided
 *      string, if it has the right signature, or a .pem-file.  In
 *      both cases, the stream must be closed by the caller via
 *      BIO_free().
 *
 * Results:
 *      OpenSSP BIO*
 *
 * Side effects:
 *      Potentially opening a file descriptor.
 *
 *----------------------------------------------------------------------
 */
static BIO *
PemOpenReadStream(const char *fnOrData)
{
    BIO *result;

    NS_NONNULL_ASSERT(fnOrData != NULL);

    if (strstr(fnOrData, "-----BEGIN ") != NULL) {
        result = BIO_new_mem_buf(fnOrData, (int)strlen(fnOrData));
    } else {
        result = BIO_new_file(fnOrData, "r");
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyGetFromPem --
 *
 *      Helper function to get pkey from PEM files
 *
 * Results:
 *      EVP_PKEY *, potentially NULL.
 *
 * Side effects:
 *      Interp result Obj is updated in case of error.
 *
 *----------------------------------------------------------------------
 */
static EVP_PKEY *
PkeyGetFromPem(Tcl_Interp *interp, const char *pemFileName, const char *passPhrase, bool private)
{
    BIO        *bio;
    EVP_PKEY   *result;

    bio = PemOpenReadStream(pemFileName);
    if (bio == NULL) {
        Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", pemFileName);
        result = NULL;
    } else {
        if (private) {
            result = PEM_read_bio_PrivateKey(bio, NULL, NULL, ns_const2voidp(passPhrase));
        } else {
            result = PEM_read_bio_PUBKEY(bio, NULL, NULL, ns_const2voidp(passPhrase));
        }
        BIO_free(bio);
        if (result == NULL) {
            Ns_TclPrintfResult(interp, "pem file contains no %s key",
                               (private ? "private" : "public"));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyGetAnyFromPem --
 *
 *      Helper function to get private or public key from PEM files
 *
 * Results:
 *      EVP_PKEY *, might be NULL
 *
 * Side effects:
 *      Interp result Obj is updated in case of error.
 *
 *----------------------------------------------------------------------
 */
static EVP_PKEY *
PkeyGetAnyFromPem(Tcl_Interp *interp, const char *pem, const char *passPhrase)
{
    EVP_PKEY *pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_FALSE);

    if (pkey == NULL) {
        pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);
    }
    return pkey;
}

# ifndef OPENSSL_NO_EC
/*
 *----------------------------------------------------------------------
 *
 * GetEckeyFromPem --
 *
 *      Helper function to get eckey from PEM files
 *
 * Results:
 *      EC_KEY *, might be NULL
 *
 * Side effects:
 *      Interp result Obj is updated in case of error.
 *
 *----------------------------------------------------------------------
 */

static EC_KEY *
GetEckeyFromPem(Tcl_Interp *interp, const char *pemFileName, const char *passPhrase, bool private)
{
    BIO        *bio;
    EC_KEY     *result;

    bio = PemOpenReadStream(pemFileName);
    if (bio == NULL) {
        Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", pemFileName);
        result = NULL;
    } else {
        if (private) {
            result = PEM_read_bio_ECPrivateKey(bio, NULL, NULL, ns_const2voidp(passPhrase));
        } else {
            result = PEM_read_bio_EC_PUBKEY(bio, NULL, NULL, ns_const2voidp(passPhrase));
        }
        BIO_free(bio);
        if (result == NULL) {
            Ns_TclPrintfResult(interp, "eckey_from_pem: pem file contains no %s EC key",
                               (private ? "private" : "public"));
        }
    }
    return result;
}
# endif /* OPENSSL_NO_EC */





/*
 *----------------------------------------------------------------------
 *
 * CryptoHmacNewObjCmd -- Subcommand of NsTclCryptoHmacObjCmd
 *
 *        Incremental command to initialize a HMAC context. This
 *        command is typically followed by a sequence of "add"
 *        subcommands until the content is read with the "get"
 *        subcommand and then freed.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Creating HMAC context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoHmacNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result, isBinary = 0;
    const char *digestName = "sha256";
    Tcl_Obj    *keyObj;
    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"digest",  Ns_ObjvString, &digestName, NULL},
        {"key",     Ns_ObjvObj,    &keyObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        NsDigest digest;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = DigestGet(interp, digestName, NS_DIGEST_USAGE_HMAC, &digest);
        if (result != TCL_ERROR) {
            HMAC_CTX            *ctx;
            const unsigned char *keyString;
            TCL_SIZE_T           keyLength;
            Tcl_DString          keyDs;

            Tcl_DStringInit(&keyDs);
            keyString = Ns_GetBinaryString(keyObj, isBinary == 1, &keyLength, &keyDs);
            ctx = HMAC_CTX_new();
            HMAC_Init_ex(ctx, keyString, (int)keyLength, digest.md, NULL);
            Ns_TclSetAddrObj(Tcl_GetObjResult(interp), hmacCtxType, ctx);
            Tcl_DStringFree(&keyDs);
            DigestFree(&digest);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoHmacAddObjCmd -- Subcommand of NsTclCryptoHmacObjCmd
 *
 *        Implements "ns_crypto::hmac add", an incremental command to
 *        add a message chunk to a predefined HMAC context, which was
 *        previously created via the "new" subcommand.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Updating HMAC context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoHmacAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, isBinary = 0;
    HMAC_CTX   *ctx;
    Tcl_Obj    *ctxObj;
    Tcl_Obj    *messageObj;
    TCL_SIZE_T  messageLength;
    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"ctx",      Ns_ObjvObj, &ctxObj, NULL},
        {"message",  Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, hmacCtxType, (void **)&ctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", hmacCtxType);
        result = TCL_ERROR;

    } else {
        const unsigned char *message;
        Tcl_DString          messageDs;

        Tcl_DStringInit(&messageDs);
        message = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);
        HMAC_Update(ctx, message, (size_t)messageLength);
        Tcl_DStringFree(&messageDs);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoHmacGetObjCmd -- Subcommand of NsTclCryptoHmacObjCmd
 *
 *        Implements "ns_crypto::hmac get", an incremental command to
 *        get the (maybe partial) HMAC result in form of a hex string.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoHmacGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK, encodingInt = -1;
    HMAC_CTX          *ctx;
    Tcl_Obj           *ctxObj;

    Ns_ObjvSpec    lopts[] = {
        {"-encoding", Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"ctx",      Ns_ObjvObj, &ctxObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, hmacCtxType, (void **)&ctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", hmacCtxType);
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        unsigned char  digest[EVP_MAX_MD_SIZE];
        char           digestChars[EVP_MAX_MD_SIZE*2 + 1];
        unsigned int   mdLength;
        HMAC_CTX      *partial_ctx;

        partial_ctx = HMAC_CTX_new();
        HMAC_CTX_copy(partial_ctx, ctx);
        HMAC_Final(partial_ctx, digest, &mdLength);
        HMAC_CTX_free(partial_ctx);

        /*
         * Convert the result to the output format and set the interp
         * result.
         */
        Tcl_SetObjResult(interp, NsEncodedObj(digest, mdLength, digestChars, encoding));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoHmacFreeObjCmd -- Subcommand of NsTclCryptoHmacObjCmd
 *
 *        Implements "ns_crypto::hmac free". Frees a previously
 *        allocated HMAC context.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Freeing memory
 *
 *----------------------------------------------------------------------
 */
static int
CryptoHmacFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int            result = TCL_OK;
    HMAC_CTX      *ctx;
    Tcl_Obj       *ctxObj;
    Ns_ObjvSpec    args[] = {
        {"ctx",  Ns_ObjvObj, &ctxObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, hmacCtxType, (void **)&ctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", hmacCtxType);
        result = TCL_ERROR;

    } else {

        HMAC_CTX_free(ctx);
        Ns_TclResetObjType(ctxObj, NULL);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoHmacStringObjCmd -- Subcommand of NsTclCryptoHmacObjCmd
 *
 *        Implements "ns_crypto::hmac string". Single command to
 *        obtain an HMAC from the provided data.  Technically, this is
 *        a combination of the other subcommands, but requires that
 *        the all data for the HMAC computation is provided in the
 *        contents of a Tcl_Obj in memory. The command returns the
 *        HMAC in form of a hex string.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoHmacStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *keyObj, *messageObj;
    const char        *digestName = "sha256";

    Ns_ObjvSpec    lopts[] = {
        {"-binary",   Ns_ObjvBool,     &isBinary,    INT2PTR(NS_TRUE)},
        {"-digest",   Ns_ObjvString,   &digestName,  NULL},
        {"-encoding", Ns_ObjvIndex,    &encodingInt, NS_binaryencodings},
        {"--",        Ns_ObjvBreak,    NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"key",     Ns_ObjvObj, &keyObj, NULL},
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        NsDigest digest;
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);

        /*
         * Look up the Message digest from OpenSSL
         */
        result = DigestGet(interp, digestName, NS_DIGEST_USAGE_HMAC, &digest);
        if (result != TCL_ERROR) {
            unsigned char        digestBytes[EVP_MAX_MD_SIZE];
            char                 digestChars[EVP_MAX_MD_SIZE*2 + 1];
            const unsigned char *keyString, *messageString;
            unsigned int         mdLength;
            TCL_SIZE_T           keyLength, messageLength;
            Tcl_DString          keyDs, messageDs;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&keyDs);
            Tcl_DStringInit(&messageDs);
            keyString = Ns_GetBinaryString(keyObj, isBinary == 1, &keyLength, &keyDs);
            messageString = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);
            hexPrint("hmac key", keyString, (size_t)keyLength);
            hexPrint("hmac message", messageString, (size_t)messageLength);

            /*
             * Call the HMAC computation.
             */
            HMAC(digest.md,
                 (const void *)keyString, (int)keyLength,
                 (const void *)messageString, (size_t)messageLength,
                 digestBytes, &mdLength);
            DigestFree(&digest);

            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            Tcl_SetObjResult(interp, NsEncodedObj(digestBytes, mdLength, digestChars, encoding));

            Tcl_DStringFree(&keyDs);
            Tcl_DStringFree(&messageDs);
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoHmacObjCmd --
 *
 *      Implements "ns_crypto::hmac" with various subcmds for handling
 *      Hash-based message authentications codes (HMAC)
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCryptoHmacObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoHmacStringObjCmd},
        {"new",     CryptoHmacNewObjCmd},
        {"add",     CryptoHmacAddObjCmd},
        {"get",     CryptoHmacGetObjCmd},
        {"free",    CryptoHmacFreeObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}




/*
 *----------------------------------------------------------------------
 *
 * CryptoMdNewObjCmd -- Subcommand of NsTclCryptoMdObjCmd
 *
 *        Implements "ns_crypto::md new". Incremental command to
 *        initialize a MD context. This command is typically followed
 *        by a sequence of "add" subcommands until the content is read
 *        with the "get" subcommand and then freed.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Creating MD context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int           result;
    const char   *digestName = "sha256";
    Ns_ObjvSpec   args[] = {
        {"digest",  Ns_ObjvString, &digestName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        NsDigest digest;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = DigestGet(interp, digestName, NS_DIGEST_USAGE_MD, &digest);
        if (result != TCL_ERROR) {
            EVP_MD_CTX    *mdctx;

            mdctx = NS_EVP_MD_CTX_new();
            EVP_DigestInit_ex(mdctx, digest.md, NULL);
            Ns_TclSetAddrObj(Tcl_GetObjResult(interp), mdCtxType, mdctx);
            DigestFree(&digest);
        }
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * CryptoMdAddObjCmd -- Subcommand of NsTclCryptoMdObjCmd
 *
 *        Implements "ns_crypto::md add". Incremental command to add a
 *        message chunk to a predefined MD context, which was
 *        previously created via the "new" subcommand.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Updating MD context.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int            result = TCL_OK, isBinary = 0;
    EVP_MD_CTX    *mdctx;
    Tcl_Obj       *ctxObj;
    Tcl_Obj       *messageObj;
    Ns_ObjvSpec    opts[] = {
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"ctx",      Ns_ObjvObj, &ctxObj, NULL},
        {"message",  Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, mdCtxType, (void **)&mdctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", mdCtxType);
        result = TCL_ERROR;

    } else {
        const unsigned char *message;
        TCL_SIZE_T           messageLength;
        Tcl_DString          messageDs;

        Tcl_DStringInit(&messageDs);
        message = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);
        EVP_DigestUpdate(mdctx, message, (size_t)messageLength);
        Tcl_DStringFree(&messageDs);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoMdGetObjCmd -- Subcommand of NsTclCryptoMdObjCmd
 *
 *        Implements "ns_crypto::md get". Incremental command to get
 *        the (maybe partial) MD result.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK, encodingInt = -1;
    EVP_MD_CTX        *mdctx;
    Tcl_Obj           *ctxObj;

    Ns_ObjvSpec lopts[] = {
        {"-encoding", Ns_ObjvIndex, &encodingInt, NS_binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"ctx", Ns_ObjvObj, &ctxObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, mdCtxType, (void **)&mdctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", mdCtxType);
        result = TCL_ERROR;

     } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        unsigned char  digest[EVP_MAX_MD_SIZE];
        char           digestChars[EVP_MAX_MD_SIZE*2 + 1];
        unsigned int   mdLength;
        EVP_MD_CTX    *partial_ctx;

        partial_ctx = NS_EVP_MD_CTX_new();
        EVP_MD_CTX_copy(partial_ctx, mdctx);
        EVP_DigestFinal_ex(partial_ctx, digest, &mdLength);
        NS_EVP_MD_CTX_free(partial_ctx);

        /*
         * Convert the result to the output format and set the interp
         * result.
         */
        Tcl_SetObjResult(interp, NsEncodedObj(digest, mdLength, digestChars, encoding));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoMdFreeObjCmd -- Subcommand of NsTclCryptoMdObjCmd
 *
 *        Implements "ns_crypto::md free". Frees a previously
 *        allocated MD context.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Freeing memory
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int            result = TCL_OK;
    EVP_MD_CTX    *mdctx;
    Tcl_Obj       *ctxObj;
    Ns_ObjvSpec    args[] = {
        {"ctx",  Ns_ObjvObj, &ctxObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, mdCtxType, (void **)&mdctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", mdCtxType);
        result = TCL_ERROR;

    } else {
        NS_EVP_MD_CTX_free(mdctx);
        Ns_TclResetObjType(ctxObj, NULL);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoMdStringObjCmd -- Subcommand of NsTclCryptoMdObjCmd
 *
 *        Implements "ns_crypto::md string", a command to obtain a MD
 *        (message digest) from the provided data.  Technically, this
 *        is a combination of the other subcommands, but requires that
 *        the all data for the MD computation is provided in the
 *        contents of a Tcl_Obj in memory. The command returns the MD
 *        in form of a hex string.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Creating HMAC context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *messageObj, *signatureObj = NULL, *resultObj = NULL;
    const char        *digestName = "sha256",
                      *passPhrase = NS_EMPTY_STRING,
                      *signKeyFile = NULL,
                      *verifyKeyFile = NULL;

    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,     &isBinary,      INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString,   &digestName,    NULL},
        {"-encoding",   Ns_ObjvIndex,    &encodingInt,   NS_binaryencodings},
        {"-passphrase", Ns_ObjvString,   &passPhrase,    NULL},
        {"-sign",       Ns_ObjvString,   &signKeyFile,   NULL},
        {"-signature",  Ns_ObjvObj,      &signatureObj,  NULL},
        {"-verify",     Ns_ObjvString,   &verifyKeyFile, NULL},
        {"--",          Ns_ObjvBreak,    NULL,           NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (signKeyFile != NULL && verifyKeyFile != NULL) {
        Ns_TclPrintfResult(interp, "the options '-sign' and '-verify' are mutually exclusive");
        result = TCL_ERROR;

    } else if ((verifyKeyFile != NULL && signatureObj == NULL)
               || (verifyKeyFile == NULL && signatureObj != NULL)
               ) {
        Ns_TclPrintfResult(interp, "the options '-verify' requires '-signature' and vice versa");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        NsDigest      digest;
        EVP_PKEY     *pkey = NULL;
        const char   *keyFile = NULL;
        bool          haveDigest = NS_FALSE;

        /*
         * Compute Message Digest or sign or validate signature via OpenSSL.
         *
         * ::ns_crypto::md string -digest sha256 -sign /usr/local/src/naviserver/private.pem "hello\n"
         *
         * Example from https://medium.com/@bn121rajesh/rsa-sign-and-verify-using-openssl-behind-the-scene-bf3cac0aade2
         * ::ns_crypto::md string -digest sha1 -sign /usr/local/src/naviserver/myprivate.pem "abcdefghijklmnopqrstuvwxyz\n"
         *
         */

        result = DigestGet(interp, digestName,
                           (signKeyFile != NULL || verifyKeyFile != NULL) ? NS_DIGEST_USAGE_SIGN_VERIFY : NS_DIGEST_USAGE_MD,
                           &digest);
        if (result == TCL_OK) {
            haveDigest = NS_TRUE;
        }

        if (signKeyFile != NULL) {
            keyFile = signKeyFile;
        } else if (verifyKeyFile != NULL) {
            keyFile = verifyKeyFile;
        }
        if (result != TCL_ERROR && keyFile != NULL) {
            pkey = PkeyGetFromPem(interp, keyFile, passPhrase, (signKeyFile != NULL));
            if (pkey == NULL) {
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR) {
            unsigned char        digestBuffer[EVP_MAX_MD_SIZE], *digestBytes = digestBuffer;
            char                 digestChars[EVP_MAX_MD_SIZE*2 + 1], *outputBuffer = digestChars;
            const unsigned char *messageString;
            TCL_SIZE_T           messageLength;
            unsigned int         mdLength = 0u;
            Tcl_DString          messageDs, signatureDs;
            bool                 haveDirectResult = NS_FALSE;

            /*
             * All input parameters are valid, get data.
             */
            Tcl_DStringInit(&messageDs);
            Tcl_DStringInit(&signatureDs);

            messageString = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);
            hexPrint("md", messageString, (size_t)messageLength);

            /*
             * Call the Digest or Signature computation
             */

            if (signKeyFile != NULL || verifyKeyFile != NULL) {
                if (signKeyFile != NULL) {
                    result = PkeySignatureSign(interp, pkey,
                                               messageString, (size_t)messageLength,
                                               NULL, 0,
                                               digest.md, encoding);
                    if (result == TCL_OK) {
                        haveDirectResult = NS_TRUE;
                    }
                } else {
                    TCL_SIZE_T           signatureLength;
                    const unsigned char *signatureString;

                    signatureString = Ns_GetBinaryString(signatureObj, 1,
                                                         &signatureLength,
                                                         &signatureDs);

                    result = PkeySignatureVerify(interp, pkey,
                                                 messageString, (size_t)messageLength,
                                                 signatureString, (size_t)signatureLength,
                                                 NULL, 0u,
                                                 digest.md);
                    if (result == TCL_OK) {
                        haveDirectResult = NS_TRUE;
                    }
                }

                EVP_PKEY_free(pkey);

            } else {
                EVP_MD_CTX *mdctx = NS_EVP_MD_CTX_new();

                EVP_DigestInit_ex(mdctx, digest.md, NULL);
                EVP_DigestUpdate(mdctx, messageString, (unsigned long)messageLength);
                EVP_DigestFinal_ex(mdctx, digestBytes, &mdLength);
                if (mdctx != NULL) {
                    NS_EVP_MD_CTX_free(mdctx);
                }
            }

            if (result == TCL_OK && !haveDirectResult) {
                /*
                 * Convert the result to the requested output format,
                 * unless we have already some resultObj.
                 */
                if (resultObj == NULL) {
                    resultObj = NsEncodedObj(digestBytes, mdLength, outputBuffer, encoding);
                }

                Tcl_SetObjResult(interp, resultObj);
            }

            if (outputBuffer != digestChars) {
                ns_free(outputBuffer);
            }
            Tcl_DStringFree(&messageDs);
            Tcl_DStringFree(&signatureDs);
        }
        if (haveDigest) {
            DigestFree(&digest);
        }
    }

    return result;
}

# ifndef OPENSSL_NO_EC
/*
 *----------------------------------------------------------------------
 *
 * CryptoMdVapidSignObjCmd -- Subcommand of NsTclCryptoMdObjCmd
 *
 *        Implements "ns_crypto::md vapidsign". Aubcommand to sign a
 *        message according to the Voluntary Application Server
 *        Identification (VAPID) for Web Push
 *        https://tools.ietf.org/id/draft-ietf-webpush-vapid-03.html
 *
 *        See also: Generic Event Delivery Using HTTP Push
 *        https://tools.ietf.org/html/rfc8030
 *
 *        Essentially, this is a special form of a signed message
 *        digest based on elliptic curve cryptography.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdVapidSignObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *messageObj;
    const char        *digestName = "sha256", *pemFile = NULL,
                      *passPhrase = NS_EMPTY_STRING;
    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,        &isBinary,    INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString,      &digestName,  NULL},
        {"-encoding",   Ns_ObjvIndex,       &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString,      &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString,      &pemFile,     NULL},
        {"--",          Ns_ObjvBreak,       NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
      set ::pemFile /usr/local/ns/modules/vapid/prime256v1_key.pem
      ::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile "hello"
      ::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile -encoding hex "hello"
      ::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile -encoding base64url "hello"

      proc vapidToken {string} {
          return $string.[::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile -encoding base64url $string]
      }

      regsub -all {[\s]} [subst {{
       "sub" : "mailto:h0325904foo@bar.com",
       "aud" : "https://updates.push.services.mozilla.com",
       "exp" : "[expr [clock seconds] + 60*120]"
      }}] "" claim
      set JWTHeader [ns_base64urlencode {{"typ":"JWT","alg":"ES256"}}]
      set JWTbody   [ns_base64urlencode $claim]

      vapidToken $JWTHeader.$JWTbody

      # check result: https://jwt.io/
    */

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        NsDigest          digest;
        bool              haveDigest = NS_FALSE;
        EC_KEY           *eckey = NULL;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = DigestGet(interp, digestName, NS_DIGEST_USAGE_MD, &digest);
        if (result != TCL_ERROR) {
            haveDigest = NS_TRUE;

            eckey = GetEckeyFromPem(interp, pemFile, passPhrase, NS_TRUE);
            if (eckey == NULL) {
                /*
                 * GetEckeyFromPem handles error message
                 */
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR) {
            unsigned char        digestBytes[EVP_MAX_MD_SIZE];
            EVP_MD_CTX          *mdctx;
            const unsigned char *messageString;
            TCL_SIZE_T           messageLength;
            unsigned int         sigLen, mdLength, rLen, sLen;
            Tcl_DString          messageDs;
            ECDSA_SIG           *sig;
            const BIGNUM        *r, *s;
            uint8_t             *rawSig;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&messageDs);
            messageString = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);

            /*
             * Call the Digest or Signature computation
             */
            mdctx = NS_EVP_MD_CTX_new();

            EVP_DigestInit_ex(mdctx, digest.md, NULL);
            EVP_DigestUpdate(mdctx, messageString, (unsigned long)messageLength);
            EVP_DigestFinal_ex(mdctx, digestBytes, &mdLength);

            sig = ECDSA_do_sign(digestBytes, (int)mdLength, eckey);
            ECDSA_SIG_get0(sig, &r, &s);
            rLen = (unsigned int) BN_num_bytes(r);
            sLen = (unsigned int) BN_num_bytes(s);
            sigLen = rLen + sLen;
            //fprintf(stderr, "siglen r %u + s%u -> %u\n", rLen, sLen, sigLen);

            rawSig = ns_calloc(sigLen, sizeof(uint8_t));
            assert(rawSig != NULL);

            BN_bn2bin(r, rawSig);
            hexPrint("r", rawSig, rLen);
            BN_bn2bin(s, &rawSig[rLen]);
            hexPrint("s", &rawSig[rLen], sLen);

            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            Tcl_SetObjResult(interp, NsEncodedObj(rawSig, sigLen, NULL, encoding));

            /*
             * Clean up.
             */
            EC_KEY_free(eckey);
            NS_EVP_MD_CTX_free(mdctx);
            ns_free(rawSig);
            Tcl_DStringFree(&messageDs);
        }
        if (haveDigest) {
            DigestFree(&digest);
        }
    }

    return result;
}
# endif /* OPENSSL_NO_EC */

# ifdef HAVE_OPENSSL_HKDF
/*
 *----------------------------------------------------------------------
 *
 * CryptoMdHkdfObjCmd -- Subcommand of NsTclCryptoMdObjCmd
 *
 *        Implements "ns_crypto::md hkdf", a command md to derive keys
 *        based on message digests.
 *
 *        See: RFC 5869: HMAC-based Extract-and-Expand Key Derivation Function (HKDF)
 *        https://tools.ietf.org/html/rfc5869
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int
CryptoMdHkdfObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, outLength = 0, encodingInt = -1;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL, *infoObj = NULL;
    const char        *digestName = "sha256";
    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,           &isBinary,    INT2PTR(NS_TRUE)},
        {"-digest",   Ns_ObjvString,         &digestName,  NULL},
        {"!-salt",    Ns_ObjvObj,            &saltObj,     NULL},
        {"!-secret",  Ns_ObjvObj,            &secretObj,   NULL},
        {"!-info",    Ns_ObjvObj,            &infoObj,     NULL},
        {"-encoding", Ns_ObjvIndex,          &encodingInt, NS_binaryencodings},
        {"--",        Ns_ObjvBreak,          NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"length", Ns_ObjvInt, &outLength, &posIntRange0},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ::ns_crypto::md hkdf -digest sha256 -salt foo -secret var -info "content-encoding: auth" 10

      # test case 1 from RFC 5869
      ::ns_crypto::md hkdf -digest sha256 \
             -salt   [binary format H* 000102030405060708090a0b0c] \
             -secret [binary format H* 0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b] \
             -info   [binary format H* f0f1f2f3f4f5f6f7f8f9] \
              42
      3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865

      # test case 3 from  RFC 5869
      ::ns_crypto::md hkdf -digest sha256 \
             -salt   "" \
             -secret [binary format H* 0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b] \
             -info   "" \
              42
      8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8

      # test case 4 from  RFC 5869
      ::ns_crypto::md hkdf -digest sha1 \
             -salt   [binary format H* 000102030405060708090a0b0c] \
             -secret [binary format H* 0b0b0b0b0b0b0b0b0b0b0b] \
             -info   [binary format H* f0f1f2f3f4f5f6f7f8f9] \
              42
      085a01ea1b10f36933068b56efa5ad81a4f14b822f5b091568a9cdd4f155fda2c22e422478d305f3f896

     */
    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        NsDigest          digest;
        bool              haveDigest = NS_FALSE;
        EVP_PKEY_CTX     *pctx = NULL;

        ERR_clear_error();

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = DigestGet(interp, digestName, NS_DIGEST_USAGE_HKDF, &digest);

        if (result != TCL_ERROR) {
            haveDigest = NS_TRUE;
            pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
            if (pctx == NULL) {
                SetResultFromOsslError(interp, "could not obtain context HKDF");
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR && (EVP_PKEY_derive_init(pctx) <= 0)) {
            SetResultFromOsslError(interp, "could not initialize for derivation");
            result = TCL_ERROR;
        }
        if (result != TCL_ERROR && (EVP_PKEY_CTX_set_hkdf_md(pctx, digest.md) <= 0)) {
            SetResultFromOsslError(interp, "could not set digest algorithm");
            result = TCL_ERROR;
        }
        if (result != TCL_ERROR) {
            const unsigned char *infoString, *saltString, *secretString;
            unsigned char       *keyString;
            Tcl_DString          infoDs, saltDs, secretDs;
            TCL_SIZE_T           infoLength, saltLength, secretLength;
            size_t               outSize = (size_t)outLength;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&saltDs);
            Tcl_DStringInit(&secretDs);
            Tcl_DStringInit(&infoDs);
            keyString = ns_malloc((size_t)outLength);

            saltString   = Ns_GetBinaryString(saltObj,   isBinary == 1, &saltLength,   &saltDs);
            secretString = Ns_GetBinaryString(secretObj, isBinary == 1, &secretLength, &secretDs);
            infoString   = Ns_GetBinaryString(infoObj,   isBinary == 1, &infoLength,   &infoDs);

            // hexPrint("salt  ", saltString,   (size_t)saltLength);
            // hexPrint("secret", secretString, (size_t)secretLength);
            // hexPrint("info  ", infoString,   (size_t)infoLength);

            if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, saltString, (int)saltLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set salt");
                result = TCL_ERROR;
            } else if (EVP_PKEY_CTX_set1_hkdf_key(pctx, secretString, (int)secretLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set secret");
                result = TCL_ERROR;
            } else if (EVP_PKEY_CTX_add1_hkdf_info(pctx, infoString, (int)infoLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set info");
                result = TCL_ERROR;
            } else if (EVP_PKEY_derive(pctx, keyString, &outSize) <= 0) {
                Ns_TclPrintfResult(interp, "could not obtain derived key");
                result = TCL_ERROR;
            }

            if (result == TCL_OK) {
                /*
                 * Convert the result to the output format and set the interp
                 * result.
                 */
                Tcl_SetObjResult(interp, NsEncodedObj(keyString, outSize, NULL, encoding));
            }

            /*
             * Clean up.
             */
            ns_free((char*)keyString);
            Tcl_DStringFree(&saltDs);
            Tcl_DStringFree(&secretDs);
            Tcl_DStringFree(&infoDs);
        }

        if (haveDigest) {
            DigestFree(&digest);
        }
        EVP_PKEY_CTX_free(pctx);
    }
    return result;
}
# endif

/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoMdObjCmd --
 *
 *      Implements "ns_crypto::md" with subcommands for Hash-based
 *      message authentication codes.
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoMdObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",    CryptoMdStringObjCmd},
        {"new",       CryptoMdNewObjCmd},
        {"add",       CryptoMdAddObjCmd},
        {"get",       CryptoMdGetObjCmd},
        {"free",      CryptoMdFreeObjCmd},
# ifdef HAVE_OPENSSL_HKDF
        {"hkdf",      CryptoMdHkdfObjCmd},
# endif
# ifndef OPENSSL_NO_EC
        {"vapidsign", CryptoMdVapidSignObjCmd},
# endif
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


# ifdef HAVE_OPENSSL_3
/*
 * We could provide SCRYPT as well via EVP_PKEY_CTX provided in
 * OpenSSL 1.1.1:
 *
 *     https://www.openssl.org/docs/man1.1.1/man7/scrypt.html
 *
 * but the future interface is the OpenSSL 3.* way, via
 * EVP_KDF_fetch() + OSSL_PARAM_*.  Not sure, whether LibreSSL and
 * friends will follow.
 */
/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoScryptObjCmd --
 *
 *      Compute a "password hash" using the scrypt Password-Based
 *      Key Derivation Function (RFC 7914) as defined in OpenSSL 3.
 *
 *      Implements "ns_crypto::scrypt".
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoScryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, nValue = 1024, rValue = 8, pValue = 16, encodingInt = -1;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,   &isBinary,   INT2PTR(NS_TRUE)},
        {"!-salt",    Ns_ObjvObj,    &saltObj,     NULL},
        {"!-secret",  Ns_ObjvObj,    &secretObj,   NULL},
        {"-n",        Ns_ObjvInt,    &nValue,      &posIntRange1},
        {"-p",        Ns_ObjvInt,    &pValue,      &posIntRange1},
        {"-r",        Ns_ObjvInt,    &rValue,      &posIntRange1},
        {"-encoding", Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {NULL, NULL, NULL, NULL}
    };
    /*
      ############################################################################
      # Test Case 1: RFC 7914 (example 2 in sect 12)
      ############################################################################
      ::ns_crypto::scrypt -secret "password" -salt NaCl -n 1024 -r 8 -p 16

      fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162
      2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640

      % time {::ns_crypto::scrypt -secret "password" -salt NaCl -n 1024 -r 8 -p 16}
      42011 microseconds per iteration

      ############################################################################
      # Test Case 2: RFC 7914 (example 3 in sect 12)
      ############################################################################
      ::ns_crypto::scrypt -secret "pleaseletmein" -salt "SodiumChloride" -n 16384 -r 8 -p 1

      7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2
      d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887

      % time {::ns_crypto::scrypt -secret "pleaseletmein" -salt "SodiumChloride" -n 16384 -r 8 -p 1}
      47901 microseconds per iteration

      ############################################################################
      # Test Case 3: RFC 7914 (example 4 in sect 12)
      ############################################################################
      ::ns_crypto::scrypt -secret "pleaselectmein" -salt SodiumChloride -n 1048576 -r 8 -p 1
      ::ns_crypto::scrypt -secret "pleaseletmein" -salt "SodiumChloride" -n 1048576 -r 8 -p 1

      2101cb9b6a511aaeaddbbe09cf70f881ec568d574a2ffd4dabe5ee9820adaa47
      8e56fd8f4ba5d09ffa1c6d927c40f4c337304049e8a952fbcbf45c6fa77a41a4

      % time {::ns_crypto::scrypt -secret "pleaseletmein" -salt "SodiumChloride" -n 1048576 -r 8 -p 1}
      3095741 microseconds per iteration
    */

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding    encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        EVP_KDF             *kdf;
        EVP_KDF_CTX         *kctx;
        unsigned char        out[64];
        Tcl_DString          saltDs, secretDs;
        TCL_SIZE_T           saltLength, secretLength;
        const unsigned char *saltString, *secretString;
        OSSL_PARAM           params[6], *p = params;
        uint64_t             nValueSSL = (uint64_t)nValue;
        uint32_t             pValueSSL = (uint32_t)pValue;
        uint32_t             rValueSSL = (uint32_t)rValue;

        /*
         * All input parameters are valid, get key and data.
         */
        Tcl_DStringInit(&saltDs);
        Tcl_DStringInit(&secretDs);

        saltString   = Ns_GetBinaryString(saltObj,   isBinary == 1, &saltLength,   &saltDs);
        secretString = Ns_GetBinaryString(secretObj, isBinary == 1, &secretLength, &secretDs);

        kdf = EVP_KDF_fetch(NULL, "SCRYPT", NULL);
        kctx = EVP_KDF_CTX_new(kdf);
        EVP_KDF_free(kdf);

        *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                                 ns_const2voidp(secretString),
                                                 (size_t)secretLength);
        *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                 ns_const2voidp(saltString),
                                                 (size_t)saltLength);
        *p++ = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_N, &nValueSSL);
        *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_SCRYPT_R, &rValueSSL);
        *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_SCRYPT_P, &pValueSSL);
        *p = OSSL_PARAM_construct_end();

        if (EVP_KDF_derive(kctx, out, sizeof(out), params) <= 0) {
            Ns_TclPrintfResult(interp, "could not derive key");
            result = TCL_ERROR;

        } else {
            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            /*printf("Output = %s\n", OPENSSL_buf2hexstr(out, sizeof(out)));*/

            Tcl_SetObjResult(interp, NsEncodedObj(out, sizeof(out), NULL, encoding));
            result = TCL_OK;
        }

        /*
         * Clean up.
         */
        Tcl_DStringFree(&saltDs);
        Tcl_DStringFree(&secretDs);

        EVP_KDF_CTX_free(kctx);
    }

    return result;
}
# else
int
NsTclCryptoScryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.0 built into NaviServer");
    return TCL_ERROR;
}
# endif

# ifdef HAVE_OPENSSL_3_2
/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoArgon2ObjCmd --
 *
 *      Compute a "password hash" using the Argon2d Password-Based
 *      Key Derivation Function (RFC 9106) as defined in OpenSSL 3.2.
 *
 *      Parameters (as defined in RFC 9106)
 *        P message string
 *        S nonce, salt
 *        T tag length
 *        p degree of parallelism (lanes)
 *        m memory size
 *        t number of passes
 *        K secret value (optional)
 *        X associated data (optional)
 *
 *      Test vectors:
 *        m 32 KiB, t 3, p 4, T 32
 *
 *      Implements "ns_crypto::argon2".
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
#include <openssl/thread.h>         /* OSSL_set_max_threads */

int
NsTclCryptoArgon2ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1,
                       memcost = 1024, iter = 3, lanes = 1, threads = 1, outlen = 64;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL, *adObj = NULL, *passObj = NULL;
    const char        *variant = "Argon2id";
    Ns_ObjvSpec lopts[] = {
        {"-ad",       Ns_ObjvObj,    &adObj,       NULL},
        {"-binary",   Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"-encoding", Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-iter",     Ns_ObjvInt,    &iter,        &posIntRange1},
        {"-lanes",    Ns_ObjvInt,    &lanes,       &posIntRange1},
        {"-memcost",  Ns_ObjvInt,    &memcost,     &posIntRange1},
        {"-outlen",   Ns_ObjvInt,    &outlen,      &posIntRange1},
        {"-password", Ns_ObjvObj,    &passObj,     NULL},
        {"!-salt",    Ns_ObjvObj,    &saltObj,     NULL},
        {"-secret",   Ns_ObjvObj,    &secretObj,   NULL},
        {"-threads",  Ns_ObjvInt,    &threads,     NULL},
        {"-variant",  Ns_ObjvString, &variant,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (threads > lanes)  {
        Ns_TclPrintfResult(interp, "requested more threads than lanes");
        result = TCL_ERROR;

    } else if (memcost < 8 * lanes) {
        Ns_TclPrintfResult(interp, "memcost must be greater or equal than 8 times the number of lanes");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding    encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        EVP_KDF             *kdf;
        EVP_KDF_CTX         *kctx = NULL;
        Tcl_DString          saltDs, secretDs, adDs, passDs, outDs;
        TCL_SIZE_T           saltLength, secretLength = 0, adLength = 0, passLength = 0;
        const unsigned char *saltString, *secretString = NULL, *adString = NULL, *passString = NULL;
        OSSL_PARAM           params[9], *p = params;
        uint32_t             memcostSSL = (uint32_t)memcost; // memory, OSSL_KDF_PARAM_ARGON2_MEMCOST
        uint32_t             iterSSL = (uint32_t)iter; // passes, OSSL_KDF_PARAM_ITER
        uint32_t             lanesSSL = (uint32_t)lanes; // lanes, OSSL_KDF_PARAM_ARGON2_LANES
        uint32_t             threadsSSL = (uint32_t)threads; // OSSL_KDF_PARAM_ARGON2_THREADS

        /*
         * All input parameters are valid, get key and data.
         */
        Tcl_DStringInit(&saltDs);
        Tcl_DStringInit(&secretDs);
        Tcl_DStringInit(&adDs);
        Tcl_DStringInit(&passDs);
        Tcl_DStringInit(&outDs);

        if (threads > 1) {
            if (OSSL_set_max_threads(NULL, threadsSSL) != 1) {
                Ns_TclPrintfResult(interp, "could not set max threads");
                result = TCL_ERROR;
                goto cleanup;
            }
            *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threadsSSL);
        }

        saltString   = Ns_GetBinaryString(saltObj,   isBinary == 1, &saltLength,   &saltDs);
        if (saltLength < 8) {
            Ns_TclPrintfResult(interp, "salt must be at least 64 bits (8 characters)");
            result = TCL_ERROR;
            goto cleanup;
        }
        //NsHexPrint("saltString", saltString, (size_t)saltLength, 32, NS_FALSE);
        *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                 ns_const2voidp(saltString),
                                                 (size_t)saltLength);

        if (secretObj != NULL) {
            secretString = Ns_GetBinaryString(secretObj, isBinary == 1, &secretLength, &secretDs);
            //NsHexPrint("secretString", secretString, (size_t)secretLength, 32, NS_FALSE);
            *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET,
                                                     ns_const2voidp(secretString),
                                                     (size_t)secretLength);
        }
        if (adObj != NULL) {
            adString = Ns_GetBinaryString(adObj,     isBinary == 1, &adLength,     &adDs);
            //NsHexPrint("adString", adString, (size_t)adLength, 32, NS_FALSE);
            *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_ARGON2_AD,
                                                     ns_const2voidp(adString),
                                                     (size_t)adLength);
        }
        if (passObj != NULL) {
            passString = Ns_GetBinaryString(passObj, isBinary == 1, &passLength,   &passDs);
            //NsHexPrint("passString", passString, (size_t)passLength, 32, NS_FALSE);
            *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                                     ns_const2voidp(passString),
                                                     (size_t)passLength);
        }

        /*fprintf(stderr, "variant %s pass (%d) secret (%d) salt (%d) threads %d iter %d lanes %d memcost %d\n",
                variant,
                passLength, secretLength, saltLength,
                threads, iterSSL, lanesSSL, memcostSSL);*/

        *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanesSSL);
        *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memcostSSL);
        *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterSSL);
        *p = OSSL_PARAM_construct_end();

        kdf = EVP_KDF_fetch(NULL, variant, NULL);
        if (kdf != NULL) {
            kctx = EVP_KDF_CTX_new(kdf);
            EVP_KDF_free(kdf);
        }
        if (kctx == NULL) {
            Ns_TclPrintfResult(interp, "argon2: could not initialize KDF context for algorithm '%s'", variant);
            result = TCL_ERROR;
            goto cleanup;
        }

        Tcl_DStringSetLength(&outDs, (TCL_SIZE_T)outlen);
        ERR_clear_error();

        if (EVP_KDF_CTX_set_params(kctx, params) <= 0) {
            SetResultFromOsslError(interp, "argon2: could not set parameters");
            result = TCL_ERROR;

        } else if (EVP_KDF_derive(kctx, (unsigned char *)outDs.string, (size_t)outlen, params) <= 0) {
            SetResultFromOsslError(interp, "argon2: could not derive key");
            result = TCL_ERROR;
        }  else {
            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            //fprintf(stderr, "Output = %s\n", OPENSSL_buf2hexstr((unsigned char *)outDs.string, outlen));

            Tcl_SetObjResult(interp, NsEncodedObj((unsigned char *)outDs.string, (size_t)outlen, NULL, encoding));
            result = TCL_OK;
        }

    cleanup:
        /*
         * Clean up.
         */
        Tcl_DStringFree(&saltDs);
        Tcl_DStringFree(&secretDs);
        Tcl_DStringFree(&adDs);
        Tcl_DStringFree(&passDs);
        Tcl_DStringFree(&outDs);

        if (kctx != NULL) {
            EVP_KDF_CTX_free(kctx);
        }
    }

    return result;
}
# else
int
NsTclCryptoArgon2ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.2 built into NaviServer");
    return TCL_ERROR;
}
# endif

/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoPbkdf2hmacObjCmd --
 *
 *      Compute a password hash using PBKDF2 (Password-Based Key
 *      Derivation Function 2). This function is used to reduce
 *      vulnerabilities of brute-force attacks against password hashes
 *      and is used e.g. in SCRAM (Salted Challenge Response
 *      Authentication Mechanism).
 *
 *      The hash function of SCRAM is PBKDF2 [RFC2898] with HMAC() as the
 *      pseudorandom function (PRF) and with dkLen == output length of
 *      HMAC() == output length of H().
 *
 *      Implements "ns_crypto::pbkdf2_hmac".
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoPbkdf2hmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1, iter = 4096, dkLength = -1;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL;
    const char        *digestName = "sha256";
    Ns_ObjvSpec opts[] = {
        {"-binary",     Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString, &digestName,  NULL},
        {"-dklen",      Ns_ObjvInt,    &dkLength,    &posIntRange1},
        {"-iterations", Ns_ObjvInt,    &iter,        &posIntRange1},
        {"!-salt",      Ns_ObjvObj,    &saltObj,     NULL},
        {"!-secret",    Ns_ObjvObj,    &secretObj,   NULL},
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ############################################################################
      # Test Cases for pbkdf2-hmac-sha1 based on RFC 6070
      # (PKCS #5_ Password-Based Key Derivation Function 2 (PBKDF2) Test Vectors)
      ############################################################################
      ::ns_crypto::pbkdf2_hmac -secret "password" -iterations 1 -salt "salt" -digest sha1
      0c60c80f961f0e71f3a9b524af6012062fe037a6

      ::ns_crypto::pbkdf2_hmac -secret "password" -iterations 2 -salt "salt" -digest sha1
      ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957

      ::ns_crypto::pbkdf2_hmac -secret "password" -iterations 4096 -salt "salt" -digest sha1
      4b007901b765489abead49d926f721d065a429c1

      ::ns_crypto::pbkdf2_hmac -secret "password" -iterations 16777216 -salt "salt" -digest sha1
      eefe3d61cd4da4e4e9945b3d6ba2158c2634e984

      ::ns_crypto::pbkdf2_hmac -secret "pass\0word" -iterations 4096 -salt "sa\0lt" -digest sha1 -dklen 16
      56fa6aa75548099dcc37d7f03425e0c3


      ############################################################################
      # Test Cases for pbkdf2-hmac-sha2 from
      * https://stackoverflow.com/questions/5130513/pbkdf2-hmac-sha2-test-vectors
      ############################################################################

      ::ns_crypto::pbkdf2_hmac -secret "password" -iterations 1 -salt "salt"
      120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b

      ::ns_crypto::pbkdf2_hmac -secret "password" -iterations 2 -salt "salt"
      ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43

      ::ns_crypto::pbkdf2_hmac -secret "password" -iterations 4096 -salt "salt"
      c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a

      ::ns_crypto::pbkdf2_hmac -secret "pass\0word" -iterations 4096 -salt "sa\0lt" -dklen 16
      89b69d0516f829893c696226650a8687

      ############################################################################
      # Performance considerations
      ############################################################################

      # PostgreSQL 10 uses 4096 (very low value)
      time {::ns_crypto::pbkdf2_hmac -secret "pass\0word" -iterations 4096 -salt "sa\0lt" -dklen 16}
      4172 microseconds per iteration

      # Recommendation from RFC 7677
      time {::ns_crypto::pbkdf2_hmac -secret "pass\0word" -iterations 15000 -salt "sa\0lt" -dklen 16}
      16027 microseconds per iteration

      # Comparison with higher value
      time {::ns_crypto::pbkdf2_hmac -secret "pass\0word" -iterations 65536 -salt "sa\0lt" -dklen 16}
      65891 microseconds per iteration
   */

    if (Ns_ParseObjv(opts, NULL, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        NsDigest          digest;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = DigestGet(interp, digestName, NS_DIGEST_USAGE_PBKDF2, &digest);
        if (result == TCL_OK) {
            Tcl_DString          saltDs, secretDs;
            TCL_SIZE_T           saltLength, secretLength;
            const unsigned char *saltString, *secretString;
            unsigned char       *out = NULL;

            /*
             * All input parameters are valid, get salt and secret
             */
            Tcl_DStringInit(&saltDs);
            Tcl_DStringInit(&secretDs);
            if (dkLength == -1) {
                dkLength = EVP_MD_size(digest.md);
            }
            out = ns_malloc((size_t)dkLength);

            saltString   = Ns_GetBinaryString(saltObj,   isBinary == 1, &saltLength,   &saltDs);
            secretString = Ns_GetBinaryString(secretObj, isBinary == 1, &secretLength, &secretDs);

            if (PKCS5_PBKDF2_HMAC((const char *)secretString, (int)secretLength,
                                  saltString, (int)saltLength,
                                  iter, digest.md,
                                  dkLength, out) == 1) {
                Tcl_SetObjResult(interp, NsEncodedObj(out, (size_t)dkLength, NULL, encoding));
                result = TCL_OK;
            } else {
                Ns_TclPrintfResult(interp, "could not derive key");
                result = TCL_ERROR;
            }
            DigestFree(&digest);
            ns_free(out);
        }
    }
    return result;
}

# ifndef OPENSSL_NO_EC
#  ifdef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * EcGroupCoordinateLength --
 *
 *      Return the expected affine coordinate length in bytes for a
 *      supported named EC group.
 *
 * Results:
 *      TCL_OK if a fixed coordinate length is known for the specified
 *      group, TCL_CONTINUE otherwise.
 *
 * Side effects:
 *      On TCL_OK, *coordLenPtr is set to the expected coordinate
 *      length.
 *
 *----------------------------------------------------------------------
 */
static int
EcGroupCoordinateLength(const char *groupName, size_t *coordLenPtr)
{
    if (STRIEQ(groupName, "prime256v1")
        || STRIEQ(groupName, "secp256r1")
        || STRIEQ(groupName, "secp256k1")) {
        *coordLenPtr = 32u;
        return TCL_OK;
    }
    if (STRIEQ(groupName, "secp384r1")) {
        *coordLenPtr = 48u;
        return TCL_OK;
    }
    if (STRIEQ(groupName, "secp521r1")) {
        *coordLenPtr = 66u;
        return TCL_OK;
    }
    return TCL_CONTINUE;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyImportEcFromCoords --
 *
 *      Construct an EC public key from affine coordinates and return it
 *      in the requested output format.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on invalid input or import failure.
 *
 * Side effects:
 *      Creates an EVP_PKEY from the provided group name and public
 *      point coordinates, and sets the interpreter result to either a
 *      PEM encoding or raw public key bytes.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyImportEcFromCoords(Tcl_Interp *interp,
                       const char *groupName,
                       const unsigned char *x, size_t xLen,
                       const unsigned char *y, size_t yLen,
                       int outputFormat,
                       const char *outfileName)
{
    int           result = TCL_ERROR;
    EVP_PKEY_CTX *ctx = NULL;
    EVP_PKEY     *pkey = NULL;
    OSSL_PARAM    params[3];
    unsigned char *pub = NULL;
    size_t         coordLen, expectedLen;

    if (xLen == 0u || yLen == 0u || xLen != yLen) {
        Ns_TclPrintfResult(interp,
                           "EC public key coordinates must be non-empty and of equal length");
        return TCL_ERROR;
    }

    if (EcGroupCoordinateLength(groupName, &expectedLen) == TCL_OK) {
        if ((size_t)xLen != expectedLen || (size_t)yLen != expectedLen) {
            Ns_TclPrintfResult(interp,
                               "invalid coordinate length for %s (need %zu bytes each)",
                               groupName, expectedLen);
            goto done;
        }
    }

    /*
     * Build SEC1 uncompressed point: 0x04 || X || Y
     */
    coordLen = xLen;
    pub = ns_malloc(1u + coordLen + coordLen);
    if (pub == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate EC public key buffer");
        return TCL_ERROR;
    }
    pub[0] = 0x04;
    memcpy(pub + 1, x, coordLen);
    memcpy(pub + 1 + coordLen, y, coordLen);

    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                 (char *)groupName, 0);
    params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                                  pub, 1u + coordLen + coordLen);
    params[2] = OSSL_PARAM_construct_end();

    ERR_clear_error();
    ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (ctx == NULL) {
        SetResultFromOsslError(interp, "could not create EC import context");
        goto done;
    }

    if (EVP_PKEY_fromdata_init(ctx) <= 0) {
        SetResultFromOsslError(interp, "could not initialize EC key import");
        goto done;
    }

    if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        SetResultFromOsslError(interp, "could not import EC public key");
        goto done;
    }

    if (outputFormat == OUTPUT_FORMAT_PEM) {
        result = PkeyPublicPemWrite(interp, pkey, "eckey", "public", outfileName);
    } else {
        result = SetResultFromRawPublicKey(interp, pkey, NS_OBJ_ENCODING_BINARY);
    }

done:
    if (pkey != NULL) {
        EVP_PKEY_free(pkey);
    }
    if (ctx != NULL) {
        EVP_PKEY_CTX_free(ctx);
    }
    if (pub != NULL) {
        ns_free(pub);
    }
    return result;
}

#  else /* HAVE_OPENSSL_3 */

static int
PkeyImportEcFromCoords(Tcl_Interp *interp,
                       const char *curveName,
                       const unsigned char *xBytes, size_t xLen,
                       const unsigned char *yBytes, size_t yLen,
                       int formatInt,
                       const char *outfileName)
{
    int             result = TCL_ERROR, nid, ok;
    int             wantPem = (formatInt == OUTPUT_FORMAT_PEM);
    EC_GROUP       *group = NULL;
    EC_POINT       *point = NULL;
    EC_KEY         *eckey = NULL;
    EVP_PKEY       *pkey = NULL;
    BIGNUM         *bx = NULL, *by = NULL;
    BN_CTX         *bn_ctx = NULL;

    /*
     * Map curve name to NID. Accept secp256r1 as alias for prime256v1.
     */
    nid = OBJ_txt2nid(curveName);
    if (nid == NID_undef) {
        if (STREQ(curveName, "secp256r1")) {
            nid = NID_X9_62_prime256v1;
        }
    }
    if (nid == NID_undef) {
        Ns_TclPrintfResult(interp, "unknown curve '%s'", curveName);
        goto done;
    }

    /*
     * Sanity checks for P-256 (WebAuthn ES256).
     */
    if (nid == NID_X9_62_prime256v1) {
        if (xLen != 32 || yLen != 32) {
            Ns_TclPrintfResult(interp,
                "invalid coordinate length for prime256v1 (need 32 bytes each)");
            goto done;
        }
    }

    /*
     * Build EC_KEY from x/y.
     */
    group = EC_GROUP_new_by_curve_name(nid);
    if (group == NULL) {
        Ns_TclPrintfResult(interp, "EC_GROUP_new_by_curve_name failed");
        goto done;
    }
    point = EC_POINT_new(group);
    if (point == NULL) {
        Ns_TclPrintfResult(interp, "EC_POINT_new failed");
        goto done;
    }
    bn_ctx = BN_CTX_new();
    if (bn_ctx == NULL) {
        Ns_TclPrintfResult(interp, "BN_CTX_new failed");
        goto done;
    }

    bx = BN_bin2bn(xBytes, (int)xLen, NULL);
    by = BN_bin2bn(yBytes, (int)yLen, NULL);
    if (bx == NULL || by == NULL) {
        Ns_TclPrintfResult(interp, "BN_bin2bn failed");
        goto done;
    }

#   if OPENSSL_VERSION_NUMBER < 0x10100000L
    ok = EC_POINT_set_affine_coordinates_GFp(group, point, bx, by, bn_ctx);
#   else
    ok = EC_POINT_set_affine_coordinates(group, point, bx, by, bn_ctx);
#   endif
    if (ok != 1) {
        Ns_TclPrintfResult(interp, "invalid EC point (cannot set affine coordinates)");
        goto done;
    }
    if (EC_POINT_is_on_curve(group, point, bn_ctx) != 1) {
        Ns_TclPrintfResult(interp, "invalid EC point (not on curve)");
        goto done;
    }

    eckey = EC_KEY_new();
    if (eckey == NULL) {
        Ns_TclPrintfResult(interp, "EC_KEY_new failed");
        goto done;
    }
    if (EC_KEY_set_group(eckey, group) != 1) {
        Ns_TclPrintfResult(interp, "EC_KEY_set_group failed");
        goto done;
    }
    if (EC_KEY_set_public_key(eckey, point) != 1) {
        Ns_TclPrintfResult(interp, "EC_KEY_set_public_key failed");
        goto done;
    }

    /*
     * Convert to EVP_PKEY and serialize as SPKI.
     */
    pkey = PkeyGetFromEcKey(interp, eckey);
    if (pkey == NULL) {
        goto done;
    }
    eckey = NULL; /* now owned by pkey */

    result = PkeyPublicWrite(interp, pkey, outfileName, wantPem);

done:
    if (eckey != NULL) { EC_KEY_free(eckey); }
    if (pkey != NULL)  { EVP_PKEY_free(pkey); }
    if (point != NULL) { EC_POINT_free(point); }
    if (group != NULL) { EC_GROUP_free(group); }
    if (bx != NULL)    { BN_free(bx); }
    if (by != NULL)    { BN_free(by); }
    if (bn_ctx != NULL){ BN_CTX_free(bn_ctx); }

    return result;
}
#  endif /* HAVE_OPENSSL_3 */
# endif /* !OPENSSL_NO_EC */


# ifndef OPENSSL_NO_EC
#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeyPrivObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Implements "ns_crypto::eckey priv". Subcommand to obtain the
 *        private key in various encodings from an elliptic curves PEM
 *        file.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoEckeyPrivObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    const char        *pemFile = NULL,
                      *passPhrase = NS_EMPTY_STRING;

    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString, &pemFile,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::eckey priv -pem /usr/local/ns/modules/vapid/prime256v1_key.pem -encoding base64url
      pwLi7T1QqrgTiNBFBLUcndjNxzx_vZiKuCcvapwjQlM
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        EVP_PKEY *pkey;
        EC_KEY   *eckey = NULL;

        pkey = PkeyGetFromPem(interp, pemFile, passPhrase, NS_TRUE);
        if (pkey == NULL) {
            /*
             * PkeyGetFromPem handles error message
             */
            result = TCL_ERROR;
        } else {
            eckey = EVP_PKEY_get1_EC_KEY(pkey);
            if (eckey == NULL) {
                EVP_PKEY_free(pkey);
                Ns_TclPrintfResult(interp, "no valid EC key in specified pem file");
                result = TCL_ERROR;
            } else {
                result = TCL_OK;
            }
        }
        if (result != TCL_ERROR) {
            Tcl_DString ds;
            size_t      octLength = EC_KEY_priv2oct(eckey, NULL, 0);

            Tcl_DStringInit(&ds);
            Tcl_DStringSetLength(&ds, (TCL_SIZE_T)octLength);
            octLength = EC_KEY_priv2oct(eckey, (unsigned char *)ds.string, octLength);
            Tcl_SetObjResult(interp, NsEncodedObj((unsigned char *)ds.string, octLength, NULL, encoding));

            /*
             * Clean up.
             */
            EVP_PKEY_free(pkey);
            Tcl_DStringFree(&ds);
        }
    }

    return result;
}
#  endif

static void
SetResultFromEC_POINT(
    Tcl_Interp       *interp,
    Tcl_DString      *dsPtr,
    EC_KEY           *eckey,
    const EC_POINT   *ecpoint,
    BN_CTX           *bn_ctx,
    Ns_BinaryEncoding encoding)
{
    size_t   octLength = EC_POINT_point2oct(EC_KEY_get0_group(eckey), ecpoint,
                                            POINT_CONVERSION_UNCOMPRESSED, NULL, 0, NULL);

    Ns_Log(Debug, "SetResultFromEC_POINT: octet length %" PRIuz, octLength);

    Tcl_DStringSetLength(dsPtr, (TCL_SIZE_T)octLength);
    octLength = EC_POINT_point2oct(EC_KEY_get0_group(eckey), ecpoint, POINT_CONVERSION_UNCOMPRESSED,
                                   (unsigned char *)dsPtr->string, octLength, bn_ctx);
    Tcl_SetObjResult(interp, NsEncodedObj((unsigned char *)dsPtr->string, octLength, NULL, encoding));
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyGetFromEcKey --
 *
 *      Wrap an EC_KEY object into an EVP_PKEY container.
 *
 *      Ownership of the EC_KEY is transferred to the returned EVP_PKEY
 *      on success. The caller must not free the EC_KEY separately after
 *      a successful call.
 *
 * Results:
 *      EVP_PKEY * on success.
 *      NULL on failure, with an error message set in the interpreter.
 *
 * Side effects:
 *      Transfers ownership of EC_KEY to EVP_PKEY.
 *      Sets interpreter result on error.
 *
 *----------------------------------------------------------------------
 */
static EVP_PKEY *
PkeyGetFromEcKey(Tcl_Interp *interp, EC_KEY *eckey)
{
    EVP_PKEY *pkey = EVP_PKEY_new();

    if (pkey == NULL) {
        Ns_TclPrintfResult(interp, "EVP_PKEY_new failed");
        return NULL;
    }

    if (EVP_PKEY_assign_EC_KEY(pkey, eckey) != 1) {
        EVP_PKEY_free(pkey);
        Ns_TclPrintfResult(interp, "EVP_PKEY_assign_EC_KEY failed");
        return NULL;
    }

    return pkey;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeyPubObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Implements "ns_crypto::eckey pub". Subcommand to obtain the
 *        public key in various encodings from an elliptic curves PEM
 *        file.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoEckeyPubObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                     TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1, formatInt = OUTPUT_FORMAT_RAW;
    const char        *pemFile = NULL, *outfileName = NULL, *passPhrase = NS_EMPTY_STRING;
    static Ns_ObjvTable formats[] = {
        {"raw", OUTPUT_FORMAT_RAW},
        {"pem", OUTPUT_FORMAT_PEM},
        {"der", OUTPUT_FORMAT_DER},
        {NULL,  0u}
    };
    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"-format",     Ns_ObjvIndex,  &formatInt,   formats},
        {"-outfile",    Ns_ObjvString, &outfileName, NULL},
        {"!-pem",       Ns_ObjvString, &pemFile,     NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
      ns_crypto::eckey pub -pem /usr/local/ns/modules/vapid/prime256v1_key.pem -encoding base64url
      BBGNrqwUWW4dedpYHZnoS8hzZZNMmO-i3nYButngeZ5KtJ73ZaGa00BZxke2h2RCRGm-6Rroni8tDPR_RMgNib0
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (formatInt == OUTPUT_FORMAT_RAW && outfileName != NULL) {
        Ns_TclPrintfResult(interp, "-outfile requires -format pem or -format der");
        result =  TCL_ERROR;

    } else if (formatInt != OUTPUT_FORMAT_RAW && encodingInt != -1) {
        Ns_TclPrintfResult(interp, "-encoding is only valid with -format raw");
        result =  TCL_ERROR;

    } else {
        EC_KEY            *eckey = NULL;
        const EC_POINT    *ecpoint = NULL;
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        /*
         * The .pem file does not have a separate pub-key included,
         * but we get the pub-key from the priv-key in form of an
         * EC_POINT.
         */
        eckey = GetEckeyFromPem(interp, pemFile, passPhrase, NS_TRUE);
        if (eckey == NULL) {
            result = TCL_ERROR;
            goto done;
        }

        ecpoint = EC_KEY_get0_public_key(eckey);
        if (ecpoint == NULL) {
            Ns_TclPrintfResult(interp, "no valid EC key in specified pem file");
            result = TCL_ERROR;
            goto done;
        }

        if (formatInt == OUTPUT_FORMAT_RAW) {
            Tcl_DString  ds;
            BN_CTX      *bn_ctx = BN_CTX_new();

            Tcl_DStringInit(&ds);
            SetResultFromEC_POINT(interp, &ds, eckey, ecpoint, bn_ctx, encoding);
            BN_CTX_free(bn_ctx);
            Tcl_DStringFree(&ds);
            result = TCL_OK;

        } else {
            EVP_PKEY *pkey = PkeyGetFromEcKey(interp, eckey);

            if (pkey == NULL) {
                result = TCL_ERROR;
                goto done;
            }
            eckey = NULL; /* now owned by pkey */

            if (formatInt == OUTPUT_FORMAT_PEM) {
                result = PkeyPublicPemWrite(interp, pkey,
                                           "EC",
                                           "EC public key",
                                           outfileName);
            } else if (formatInt == OUTPUT_FORMAT_DER) {
                result = PkeyPublicWrite(interp, pkey, outfileName, NS_FALSE);
            } else {
                Ns_TclPrintfResult(interp, "unexpected format code");
                result = TCL_ERROR; /* should not happen */
            }
            EVP_PKEY_free(pkey);
        }
    done:
        if (eckey != NULL) {
            EC_KEY_free(eckey);
        }
    }

    return result;
}


#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeyImportObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Implements "ns_crypto::eckey import". Subcommand to import a
 *        public key into the OpenSSL EC_KEY structure in order to
 *        apply conversions of it. Can be most likely dropped.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoEckeyImportObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *importObj = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,  &isBinary,    INT2PTR(NS_TRUE)},
        {"!-string",  Ns_ObjvObj,   &importObj,   NULL},
        {"-encoding", Ns_ObjvIndex, &encodingInt, NS_binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::eckey import -encoding base64url \
          -string [ns_base64urldecode BBGNrqwUWW4dedpYHZnoS8hzZZNMmO-i3nYButngeZ5KtJ73ZaGa00BZxke2h2RCRGm-6Rroni8tDPR_RMgNib0]

      ns_crypto::eckey import -encoding base64url \
          -string [ns_base64urldecode BDwwYm4O5dZG9SO6Vaz168iDLGWMmitkj5LFvunvMfgmI2fZdAEaiHTDfKR0fvr0D3V56cSGSeUwP0xNdrXho5k]
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        TCL_SIZE_T           rawKeyLength;
        const unsigned char *rawKeyString;
        EC_KEY              *eckey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        Tcl_DString          keyDs;
        Ns_BinaryEncoding    encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);

        Tcl_DStringInit(&keyDs);
        rawKeyString = Ns_GetBinaryString(importObj, isBinary == 1, &rawKeyLength, &keyDs);

        Ns_Log(Debug, "import: raw key length %" PRITcl_Size, rawKeyLength);
        hexPrint("key", rawKeyString, (size_t)rawKeyLength);

        if (EC_KEY_oct2key(eckey, rawKeyString, (size_t)rawKeyLength, NULL) != 1) {
            Ns_TclPrintfResult(interp, "could not import string to ec key");
            result = TCL_ERROR;
        } else {
            Tcl_DString  ds;
            const EC_POINT *ecpoint = EC_KEY_get0_public_key(eckey);

            Tcl_DStringInit(&ds);
            if (ecpoint == NULL) {
                Ns_TclPrintfResult(interp, "no valid public key");
                result = TCL_ERROR;
            } else {
                BN_CTX  *bn_ctx = BN_CTX_new();

                SetResultFromEC_POINT(interp, &ds, eckey, ecpoint, bn_ctx, encoding);
                BN_CTX_free(bn_ctx);

                result = TCL_OK;
            }
            Tcl_DStringFree(&ds);
        }

        /*
         * Clean up.
         */
        if (eckey != NULL) {
            EC_KEY_free(eckey);
        }
        Tcl_DStringFree(&keyDs);
    }

    return result;
}
#  endif /* HAVE_OPENSSL_EC_PRIV2OCT */


/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeyFromCoordsObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Implements "ns_crypto::eckey fromcoords". Construct an EC public
 *        key from affine coordinates (x/y) for a given curve and return
 *        the public key in PEM (SubjectPublicKeyInfo) or DER form.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoEckeyFromCoordsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, formatInt = OUTPUT_FORMAT_PEM;
    const char        *curveName = NULL, *outfileName = NULL;
    Tcl_Obj           *xObj = NULL, *yObj = NULL;
    static Ns_ObjvTable formats[] = {
        {"pem",       OUTPUT_FORMAT_PEM},
        {"der",       OUTPUT_FORMAT_DER},
        {NULL,        0u}
    };
    Ns_ObjvSpec lopts[] = {
        {"-binary",    Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"!-curve",    Ns_ObjvString, &curveName,   NULL},
        {"!-x",        Ns_ObjvObj,    &xObj,        NULL},
        {"!-y",        Ns_ObjvObj,    &yObj,        NULL},
        {"-format",    Ns_ObjvIndex,  &formatInt,   formats},
        {"-outfile",   Ns_ObjvString, &outfileName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const unsigned char *xBytes, *yBytes;
        TCL_SIZE_T           xLen = 0, yLen = 0;
        Tcl_DString          xDs, yDs;

        Tcl_DStringInit(&xDs);
        Tcl_DStringInit(&yDs);

        xBytes = (const unsigned char *)Ns_GetBinaryString(xObj, isBinary == 1, &xLen, &xDs);
        yBytes = (const unsigned char *)Ns_GetBinaryString(yObj, isBinary == 1, &yLen, &yDs);

        if (xBytes == NULL || yBytes == NULL) {
            Ns_TclPrintfResult(interp, "could not obtain coordinates");
            result = TCL_ERROR;
        } else {
            result = PkeyImportEcFromCoords(interp, curveName,
                                            xBytes, (size_t)xLen,
                                            yBytes, (size_t)yLen,
                                            formatInt, outfileName);
        }

        Tcl_DStringFree(&xDs);
        Tcl_DStringFree(&yDs);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeyGenerateObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Implements "ns_crypto::eckey generate". Subcommand to
 *        generate an EC pemfile without the need of an external
 *        command.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoEckeyGenerateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, nid;
    const char        *curvenameString = "prime256v1", *pemFileName = NULL, *outfileName = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-name",    Ns_ObjvString, &curvenameString, NULL},
        {"-pem",     Ns_ObjvString, &pemFileName,     NULL},
        {"-outfile", Ns_ObjvString, &outfileName,     NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
     * ns_crypto::eckey generate -name prime256v1 -outfile /tmp/foo.pem
     */
    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (pemFileName != NULL && outfileName != NULL) {
        Ns_TclPrintfResult(interp, "specify either '-outfile' or '-pem' (legacy), but not both");
        result = TCL_ERROR;

    } else if (CurveNidGet(interp, curvenameString, &nid) == TCL_ERROR) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        EC_KEY *eckey = EC_KEY_new_by_curve_name(nid);

        if (pemFileName != NULL) {
            // todo: warn about usage of legacy name in the future (maybe in 5.2?)
            outfileName = pemFileName;
        }

        if (eckey == NULL) {
            Ns_TclPrintfResult(interp, "could not create ec key");
            result = TCL_ERROR;

        } else if (EC_KEY_generate_key(eckey) == 0) {
            Ns_TclPrintfResult(interp, "could not generate ec key");
            result = TCL_ERROR;

        } else {
            BIO *bio = PemOpenWriteStream(interp, outfileName);

            if (bio == NULL) {
                result = TCL_ERROR;

            } else if (PEM_write_bio_ECPrivateKey(bio, eckey, NULL, NULL, 0, NULL, NULL) != 1) {
                Ns_TclPrintfResult(interp,
                                   "could not write EC key \"%s\"",
                                   curvenameString);
                result = TCL_ERROR;
            } else {
                result = PemWriteResult(interp, bio, outfileName, "EC");
            }

            if (bio != NULL) {
                BIO_free(bio);
            }
        }
        EC_KEY_free(eckey);
    }

    return result;
}

#  ifndef HAVE_OPENSSL_PRE_1_1
/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeySharedsecretObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Implements "ns_crypto::eckey sharedsecret". Subcommand to
 *        generate a shared secret based on the private key from the
 *        .pem file and the provided public key.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoEckeySharedsecretObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    const char        *pemFileName = NULL,
                      *passPhrase = NS_EMPTY_STRING;
    Tcl_Obj           *pubkeyObj = NULL;
    EC_KEY            *eckey = NULL;

    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",        Ns_ObjvString, &pemFileName, NULL},
        {"--",          Ns_ObjvBreak,  NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"pubkey", Ns_ObjvObj, &pubkeyObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
      ns_crypto::eckey sharedsecret -pem /usr/local/ns/modules/vapid/prime256v1_key.pem \
        [ns_base64urldecode BBGNrqwUWW4dedpYHZnoS8hzZZNMmO-i3nYButngeZ5KtJ73ZaGa00BZxke2h2RCRGm-6Rroni8tDPR_RMgNib0]
    */

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        eckey = GetEckeyFromPem(interp, pemFileName, passPhrase, NS_TRUE);
        if (eckey == NULL) {
            /*
             * GetEckeyFromPem handles error message
             */
            result = TCL_ERROR;
        } else {
            result = TCL_OK;
        }
    }

    if (result == TCL_OK) {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        TCL_SIZE_T           pubkeyLength;
        const unsigned char *pubkeyString;
        Tcl_DString          importDs;
        const EC_GROUP      *group;
        EC_POINT            *pubKeyPt;
        BN_CTX              *bn_ctx = BN_CTX_new();

        /*
         * Ingredients:
         *  eckey       : private key, from PEM, EC_KEY (currently redundant)
         *  pubkeyString: public key of peer as octet string
         */

        Tcl_DStringInit(&importDs);
        pubkeyString = Ns_GetBinaryString(pubkeyObj, isBinary == 1, &pubkeyLength, &importDs);

        /*
          ns_crypto::eckey generate -name prime256v1 -pem /tmp/prime256v1_key.pem
          ns_crypto::eckey sharedsecret -pem /tmp/prime256v1_key.pem [ns_base64urldecode BBGNrqwUWW4dedpYHZnoS8hzZZNMmO-i3nYButngeZ5KtJ73ZaGa00BZxke2h2RCRGm-6Rroni8tDPR_RMgNib0]
        */

        group = EC_KEY_get0_group(eckey);

        /*
         * Computes the ECDH shared secret, used as the input key material (IKM) for
         * HKDF.
         */

        pubKeyPt = EC_POINT_new(group);

        if (EC_POINT_oct2point(group, pubKeyPt, pubkeyString, (size_t)pubkeyLength, bn_ctx) != 1) {
            Ns_TclPrintfResult(interp, "could not derive EC point from provided key");
            result = TCL_ERROR;

        } else {
            size_t      sharedSecretLength;
            Tcl_DString ds;

            Tcl_DStringInit(&ds);
            sharedSecretLength = (size_t)((EC_GROUP_get_degree(group) + 7) / 8);
            Tcl_DStringSetLength(&ds, (TCL_SIZE_T)sharedSecretLength);

            if (ECDH_compute_key(ds.string, sharedSecretLength, pubKeyPt, eckey, NULL) <= 0) {
                Ns_TclPrintfResult(interp, "could not derive shared secret");
                result = TCL_ERROR;

            } else {
                /*
                 * Success: we were able to convert the octets to EC
                 * points and to compute a shared secret from this. So
                 * we can return the shared secret in the requested
                 * encoding.
                 */
                /* hexPrint("ecec       ", (unsigned char *)ds.string, sharedSecretLength);*/
                Tcl_SetObjResult(interp, NsEncodedObj((unsigned char *)ds.string, sharedSecretLength, NULL, encoding));
            }
            Tcl_DStringFree(&ds);
        }
        /*
         * Clean up.
         */
        BN_CTX_free(bn_ctx);
        EC_POINT_free(pubKeyPt);
        Tcl_DStringFree(&importDs);

        if (eckey != NULL) {
            EC_KEY_free(eckey);
        }
    }

    return result;
}
#  endif


/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoEckeyObjCmd --
 *
 *      Implements "ns_crypto::eckey" with various subcommands to
 *      provide subcommands to handle EC (elliptic curve) cryptography
 *      related commands.
 *
 * Results:
 *      Tcl Return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoEckeyObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"generate",     CryptoEckeyGenerateObjCmd},
        {"pub",          CryptoEckeyPubObjCmd},
        {"fromcoords",   CryptoEckeyFromCoordsObjCmd},
#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
        {"import",       CryptoEckeyImportObjCmd},
        {"priv",         CryptoEckeyPrivObjCmd},
#  endif
#  ifndef HAVE_OPENSSL_PRE_1_1
        {"sharedsecret", CryptoEckeySharedsecretObjCmd},
#  endif
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
# endif /* OPENSSL_NO_EC */


/*======================================================================
 * Function Implementations: KEM (key encapsulation mechanism) support
 *======================================================================
 */

# ifdef HAVE_OPENSSL_3_5

/*
 *----------------------------------------------------------------------
 *
 * CryptoKemGenerateObjCmd -- Subcommand of NsTclCryptoKemObjCmd
 *
 *      Implements "ns_crypto::kem generate". Subcommand to generate an
 *      KEM private key in PEM format without the need of an external
 *      command.
 *
 *      When -pem is specified, the generated key is written to the
 *      provided file. Otherwise, the generated PEM string is returned
 *      as Tcl result.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      Creates a KEM key pair and either writes it to a file or
 *      returns it as PEM string.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKemGenerateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                        TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const char *nameString = "ml-kem-768", *groupName = NULL, *outfileName = NULL;
    OSSL_PARAM  params[2], *paramPtr = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-name",    Ns_ObjvString, &nameString,  NULL},
        {"-group",   Ns_ObjvString, &groupName,   NULL},
        {"-outfile", Ns_ObjvString, &outfileName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
      ns_crypto::kem generate -name ml-kem-768 -pem /tmp/mlkem.pem
      ns_crypto::kem generate -name ml-kem-768
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else if (KeygenGroupParams(interp, nameString, groupName,
                                 "key encapsulation", params, &paramPtr) != TCL_OK) {
        return TCL_ERROR;
    }

    return GeneratePrivateKeyPem(interp,
                                 nameString,
                                 "key encapsulation",
                                 outfileName,
                                 NS_CRYPTO_KEYGEN_USAGE_KEM,
                                 paramPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoKemPubObjCmd -- Subcommand of NsTclCryptoKemObjCmd
 *
 *      Implements "ns_crypto::kem pub". Subcommand to obtain the
 *      public key in various encodings from an ML-KEM PEM file or
 *      PEM string.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKemPubObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                   TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result;
    const char        *pem = NULL, *outfileName = NULL,
                      *passPhrase = NS_EMPTY_STRING;
    Ns_ObjvSpec lopts[] = {
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"-outfile",    Ns_ObjvString, &outfileName, NULL},
        {"!-pem",       Ns_ObjvString, &pem,        NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        EVP_PKEY *pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);

        if (pkey == NULL) {
            result = TCL_ERROR;

        } else if (!PkeySupportsKem(pkey)) {
            Ns_TclPrintfResult(interp, "provided PEM does not support key encapsulation");
            EVP_PKEY_free(pkey);
            result = TCL_ERROR;

        } else {
            result = PkeyPublicPemWrite(interp,
                                       pkey,
                                       "KEM",
                                       "generated KEM public key",
                                       outfileName);
            EVP_PKEY_free(pkey);
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyKemEncapsulate --
 *
 *      Perform ML-KEM encapsulation using the provided EVP_PKEY and
 *      return the resulting ciphertext and shared secret.
 *
 *      The function initializes an OpenSSL EVP_PKEY_CTX for the given
 *      key, performs a two-step encapsulation to determine the required
 *      output buffer sizes, allocates the buffers, and then computes
 *      the ciphertext and shared secret via EVP_PKEY_encapsulate().
 *
 *      The results are returned as a Tcl dictionary with the keys
 *      "ciphertext" and "secret". Both values are encoded according to
 *      the specified Ns_BinaryEncoding (e.g., hex, base64, binary).
 *
 * Results:
 *      TCL_OK on success. The interpreter result is set to a dictionary:
 *
 *          {ciphertext <encoded-ciphertext> secret <encoded-secret>}
 *
 *      TCL_ERROR on failure. In this case, a descriptive error message
 *      is stored in the interpreter result. Errors may occur when:
 *
 *        - the EVP_PKEY_CTX cannot be created,
 *        - encapsulation initialization fails,
 *        - output buffer sizes cannot be determined,
 *        - memory allocation fails, or
 *        - the encapsulation operation itself fails.
 *
 * Side effects:
 *      Allocates temporary buffers for ciphertext and shared secret.
 *      These buffers are freed before returning. The shared secret is
 *      cleared with OPENSSL_clear_free() to avoid leaving sensitive
 *      material in memory.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyKemEncapsulate(Tcl_Interp *interp, EVP_PKEY *pkey, Ns_BinaryEncoding encoding)
{
    int            result = TCL_ERROR;
    EVP_PKEY_CTX  *ctx = NULL;
    unsigned char *ciphertext = NULL;
    unsigned char *secret = NULL;
    size_t         ciphertextLen = 0u;
    size_t         secretLen = 0u;

    ERR_clear_error();

    ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    if (ctx == NULL) {
        SetResultFromOsslError(interp, "could not create KEM context");
        goto done;
    }

    if (EVP_PKEY_encapsulate_init(ctx, NULL) <= 0) {
        SetResultFromOsslError(interp, "could not initialize KEM encapsulation");
        goto done;
    }

    if (EVP_PKEY_encapsulate(ctx, NULL, &ciphertextLen, NULL, &secretLen) <= 0) {
        SetResultFromOsslError(interp, "could not determine KEM output lengths");
        goto done;
    }

    ciphertext = OPENSSL_malloc(ciphertextLen);
    secret     = OPENSSL_malloc(secretLen);
    if (ciphertext == NULL || secret == NULL) {
        SetResultFromOsslError(interp, "could not allocate KEM output buffers");
        goto done;
    }

    if (EVP_PKEY_encapsulate(ctx,
                             ciphertext, &ciphertextLen,
                             secret, &secretLen) <= 0) {
        Ns_TclPrintfResult(interp, "could not encapsulate shared secret");
        goto done;
    }

    {
        Tcl_Obj *dictObj = Tcl_NewDictObj();

        Tcl_DictObjPut(interp, dictObj,
                       NsAtomObj(NS_ATOM_CIPHERTEXT),
                       NsEncodedObj(ciphertext, ciphertextLen, NULL, encoding));
        Tcl_DictObjPut(interp, dictObj,
                       NsAtomObj(NS_ATOM_SECRET),
                       NsEncodedObj(secret, secretLen, NULL, encoding));

        Tcl_SetObjResult(interp, dictObj);
    }

    result = TCL_OK;

done:
    if (secret != NULL) {
        OPENSSL_clear_free(secret, secretLen);
    }
    if (ciphertext != NULL) {
        OPENSSL_free(ciphertext);
    }
    if (ctx != NULL) {
        EVP_PKEY_CTX_free(ctx);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoKemEncapsulateObjCmd -- Subcommand of NsTclCryptoKemObjCmd
 *
 *      Implements "ns_crypto::kem encapsulate". Subcommand to
 *      encapsulate a shared secret for an ML-KEM public key.
 *
 *      The public key can be provided either directly via -pub or
 *      indirectly via -pem. When -pem is used, the public key is
 *      obtained from the provided private key.
 *
 *      The result is a two-element Tcl list containing the ciphertext
 *      and the shared secret, both encoded according to -encoding.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKemEncapsulateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                           TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    const char        *pem = NULL;
    const char        *passPhrase = NS_EMPTY_STRING;
    EVP_PKEY          *pkey = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    /*
      set pem [ns_crypto::kem generate]
      ns_crypto::kem encapsulate -pem $pem
      ns_crypto::kem encapsulate -pem $pem -encoding base64url
    */

    pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_FALSE);
    if (pkey == NULL) {
        pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);
        if (pkey == NULL) {
            return TCL_ERROR;
        }
    }
    if (!PkeySupportsKem(pkey)) {
        Ns_TclPrintfResult(interp, "provided PEM does not support key encapsulation");
        result = TCL_ERROR;
        goto done;
    }

    result = PkeyKemEncapsulate(interp, pkey,
                            (encodingInt == -1 ? NS_OBJ_ENCODING_HEX: (Ns_BinaryEncoding)encodingInt));
 done:
    EVP_PKEY_free(pkey);
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoKemDecapsulateObjCmd -- Subcommand of NsTclCryptoKemObjCmd
 *
 *      Implements "ns_crypto::kem decapsulate". Subcommand to
 *      decapsulate a shared secret from an ML-KEM ciphertext using
 *      the provided private key.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKemDecapsulateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                           TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    Tcl_Obj           *ciphertextObj = NULL;
    const char        *pem = NULL;
    const char        *passPhrase = NS_EMPTY_STRING;
    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt,   NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,    NULL},
        {"!-pem",       Ns_ObjvString, &pem,           NULL},
        {"!-ciphertext",Ns_ObjvObj,    &ciphertextObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::kem decapsulate -pem $pem -ciphertext $ciphertext
      ns_crypto::kem decapsulate -pem $pem -ciphertext $ciphertext -encoding base64url
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else {
        Ns_BinaryEncoding    encoding = (encodingInt == -1
                                         ? NS_OBJ_ENCODING_HEX
                                         : (Ns_BinaryEncoding)encodingInt);
        EVP_PKEY            *pkey = NULL;
        EVP_PKEY_CTX        *ctx = NULL;
        const unsigned char *ciphertextString;
        TCL_SIZE_T           ciphertextLength;
        unsigned char       *secret = NULL;
        size_t               secretLen = 0u, secretAllocLen = 0u;

        pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);
        if (pkey == NULL) {
            result = TCL_ERROR;
            goto done;
        }

        ciphertextString = (const unsigned char *)Tcl_GetByteArrayFromObj(ciphertextObj,
                                                                          &ciphertextLength);
        ERR_clear_error();

        ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
        if (ctx == NULL) {
            SetResultFromOsslError(interp, "could not create KEM context");
            result = TCL_ERROR;
            goto done;
        }

        if (EVP_PKEY_decapsulate_init(ctx, NULL) <= 0) {
            SetResultFromOsslError(interp, "could not initialize KEM decapsulation");
            result = TCL_ERROR;
            goto done;
        }

        if (EVP_PKEY_decapsulate(ctx, NULL, &secretAllocLen,
                                 ciphertextString, (size_t)ciphertextLength) <= 0) {
            SetResultFromOsslError(interp, "could not determine KEM shared secret length");
            result = TCL_ERROR;
            goto done;
        }

        secret = OPENSSL_malloc(secretAllocLen);
        if (secret == NULL) {
            SetResultFromOsslError(interp, "could not allocate KEM output buffer");
            result = TCL_ERROR;
            goto done;
        }

        secretLen = secretAllocLen;
        if (EVP_PKEY_decapsulate(ctx, secret, &secretLen,
                                 ciphertextString, (size_t)ciphertextLength) <= 0) {
            SetResultFromOsslError(interp, "could not decapsulate shared secret");
            result = TCL_ERROR;
            goto done;
        }

        Tcl_SetObjResult(interp, NsEncodedObj(secret, secretLen, NULL, encoding));
        result = TCL_OK;

    done:
        if (secret != NULL) {
            OPENSSL_clear_free(secret, secretLen);
        }
        if (ctx != NULL) {
            EVP_PKEY_CTX_free(ctx);
        }
        if (pkey != NULL) {
            EVP_PKEY_free(pkey);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoKemObjCmd --
 *
 *      Implements "ns_crypto::kem" with various subcommands to
 *      provide subcommands for key encapsulation and related
 *      commands.
 *
 * Results:
 *      Tcl Return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoKemObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"generate",    CryptoKemGenerateObjCmd},
        {"pub",         CryptoKemPubObjCmd},
        {"encapsulate", CryptoKemEncapsulateObjCmd},
        {"decapsulate", CryptoKemDecapsulateObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
# endif /* HAVE_OPENSSL_3_5 */

/*======================================================================
 * Function Implementations: ns_crypto::agreement
 *======================================================================
 */


# ifdef HAVE_OPENSSL_3
/*----------------------------------------------------------------------
 *
 * CryptoAgreementGenerateObjCmd --
 *
 *      Implements "ns_crypto::agreement generate". Generate a private key
 *      for key agreement in PEM format.
 *
 *      The key type is selected via "-name" and resolved through
 *      OpenSSL's provider-based key management layer.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      Creates a key pair and either writes it to a file or returns
 *      it as PEM string.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoAgreementGenerateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                        TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const char *typeName = "X25519", *outfileName = NULL, *groupName = NULL;
    OSSL_PARAM  params[2], *paramPtr = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-name",    Ns_ObjvString, &typeName,  NULL},
        {"-group",   Ns_ObjvString, &groupName,   NULL},
        {"-outfile", Ns_ObjvString, &outfileName, NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::agreement generate -name X25519 -outfile /tmp/x25519.pem
      ns_crypto::agreement generate -name X448
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else if (KeygenGroupParams(interp, typeName, groupName,
                                 "key agreement", params, &paramPtr) != TCL_OK) {
        return TCL_ERROR;

    } else {
        return GeneratePrivateKeyPem(interp,
                                     typeName,
                                     "key agreement",
                                     outfileName,
                                     NS_CRYPTO_KEYGEN_USAGE_AGREEMENT,
                                     paramPtr);
    }
}

/*----------------------------------------------------------------------
 *
 * CryptoAgreementPubObjCmd --
 *
 *      Implements "ns_crypto::agreement pub".
 *
 *      Extract the public key corresponding to a
 *      key agreement key in PEM format.
 *
 *      The public key is returned or written as PEM encoded
 *      SubjectPublicKeyInfo ("BEGIN PUBLIC KEY").
 *
 * Results:
 *      TCL_OK on success.
 *      TCL_ERROR on failure.
 *
 * Side effects:
 *      Reads key material and may write to a file or set interpreter
 *      result.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoAgreementPubObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                   TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result;
    const char *pem = NULL;
    const char *passPhrase = NS_EMPTY_STRING;
    const char *outfileName = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"-outfile",    Ns_ObjvString, &outfileName, NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::agreement pub -pem /tmp/x25519.pem -outfile /tmp/x25519-pub.pem
      ns_crypto::agreement pub -pem $pemString
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    /*
     * Accept a private key PEM and derive/export the corresponding public key.
     */
    {
        EVP_PKEY *pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);

        if (pkey == NULL) {
            result = TCL_ERROR;

        } else if (!PkeySupportsAgreement(pkey)) {
            Ns_TclPrintfResult(interp,
                               "provided PEM does not support key agreement");
            EVP_PKEY_free(pkey);
            result = TCL_ERROR;

        } else {
            result = PkeyPublicPemWrite(interp,
                                       pkey,
                                       "key agreement",
                                       "public",
                                       outfileName);
            EVP_PKEY_free(pkey);
        }
    }

    return result;
}

/*----------------------------------------------------------------------
 *
 * CryptoAgreementDeriveObjCmd --
 *
 *      Implements "ns_crypto::agreement derive".
 *
 *      Derive a shared secret from a private key and a peer public key
 *      using a key agreement algorithm such as X25519,
 *      X448, DH, or EC-based ECDH.
 *
 *      The local key is provided via "-pem" and must contain a private
 *      key. The peer key is provided via "-peer" and must contain a
 *      public key.
 *
 * Results:
 *      TCL_OK on success, with the derived secret returned in the
 *      requested encoding.
 *      TCL_ERROR on failure, with an error message set in the
 *      interpreter.
 *
 * Side effects:
 *      Reads key material from PEM input and sets the Tcl interpreter
 *      result.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoAgreementDeriveObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                      TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    const char        *pem = NULL;
    const char        *peerPem = NULL;
    const char        *passPhrase = NS_EMPTY_STRING;
    EVP_PKEY          *pkey = NULL;
    EVP_PKEY          *peerPkey = NULL;
    EVP_PKEY_CTX      *ctx = NULL;
    Ns_BinaryEncoding  encoding;
    unsigned char     *secret = NULL;
    size_t             secretLen = 0u;
    Ns_ObjvSpec        lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {"!-peer",      Ns_ObjvString, &peerPem,     NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    encoding = (encodingInt == -1
                ? NS_OBJ_ENCODING_HEX
                : (Ns_BinaryEncoding)encodingInt);

    /*
     * Local key must be a private key.
     */
    pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);
    if (pkey == NULL) {
        result = TCL_ERROR;
        goto done;
    }

    if (!PkeySupportsAgreement(pkey)) {
        Ns_TclPrintfResult(interp,
                           "provided PEM does not support key agreement");
        result = TCL_ERROR;
        goto done;
    }

    /*
     * Peer key must be a public key.
     */
    peerPkey = PkeyGetFromPem(interp, peerPem, NS_EMPTY_STRING, NS_FALSE);
    if (peerPkey == NULL) {
        result = TCL_ERROR;
        goto done;
    }

    if (!PkeySupportsAgreement(peerPkey)) {
        Ns_TclPrintfResult(interp,
                           "provided peer key does not support key agreement");
        result = TCL_ERROR;
        goto done;
    }

    ERR_clear_error();

    ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    if (ctx == NULL) {
        SetResultFromOsslError(interp, "could not create key agreement context");
        result = TCL_ERROR;
        goto done;
    }

    if (EVP_PKEY_derive_init(ctx) <= 0) {
        SetResultFromOsslError(interp, "could not initialize key agreement");
        result = TCL_ERROR;
        goto done;
    }

    if (EVP_PKEY_derive_set_peer(ctx, peerPkey) <= 0) {
        Ns_TclPrintfResult(interp,
                           "private and peer keys are not compatible for key agreement");
        result = TCL_ERROR;
        goto done;
    }

    if (EVP_PKEY_derive(ctx, NULL, &secretLen) <= 0) {
        SetResultFromOsslError(interp, "could not determine derived secret length");
        result = TCL_ERROR;
        goto done;
    }

    secret = ns_malloc(secretLen);
    if (secret == NULL) {
        Ns_TclPrintfResult(interp,
                           "could not allocate derived secret buffer");
        result = TCL_ERROR;
        goto done;
    }

    if (EVP_PKEY_derive(ctx, secret, &secretLen) <= 0) {
        Ns_TclPrintfResult(interp,
                           "could not derive shared secret");
        result = TCL_ERROR;
        goto done;
    }

    Tcl_SetObjResult(interp,
                     NsEncodedObj(secret, secretLen, NULL, encoding));
    result = TCL_OK;

done:
    if (secret != NULL) {
        ns_free(secret);
    }
    if (ctx != NULL) {
        EVP_PKEY_CTX_free(ctx);
    }
    if (peerPkey != NULL) {
        EVP_PKEY_free(peerPkey);
    }
    if (pkey != NULL) {
        EVP_PKEY_free(pkey);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoAgreementObjCmd --
 *
 *      Implements "ns_crypto::agreement" with various subcommands to
 *      provide subcommands for key agreement algorithms.
 *
 * Results:
 *      Tcl Return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoAgreementObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"generate",    CryptoAgreementGenerateObjCmd},
        {"pub",         CryptoAgreementPubObjCmd},
        {"derive",      CryptoAgreementDeriveObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
# endif /* HAVE_OPENSSL_3 */

/*======================================================================
 * Function Implementations: ns_crypto::aead support
 *======================================================================
 */


/*
 *----------------------------------------------------------------------
 *
 * CryptoAeadStringGetArguments --
 *
 *        Helper function for CryptoAeadEncryptStringObjCmd and
 *        CryptoAeadDecryptStringObjCmd.  The argument passing for
 *        both functions is very similar and is quite long. This
 *        function factors out communalities to avoid code
 *        duplication. It should be possible to reuse this function
 *        largely when we support incremental updates similar to
 *        "ns_md" and "ns_hmac".
 *
 * Results:
 *      Tcl Result Code (and many output arguments)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int
CryptoAeadStringGetArguments(
    Tcl_Interp           *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, bool encrypt,
    Tcl_DString          *ivDsPtr, Tcl_DString *keyDsPtr, Tcl_DString *aadDsPtr,
    Tcl_DString          *tagDsPtr, Tcl_DString *inputDsPtr,
    const unsigned char **keyStringPtr,   TCL_SIZE_T *keyLengthPtr,
    const unsigned char **ivStringPtr,    TCL_SIZE_T *ivLengthPtr,
    const unsigned char **aadStringPtr,   TCL_SIZE_T *aadLengthPtr,
    const char          **tagStringPtr,   TCL_SIZE_T *tagLengthPtr,
    const unsigned char **inputStringPtr, TCL_SIZE_T *inputLengthPtr,
    const EVP_CIPHER    **cipherPtr, Ns_BinaryEncoding *encodingPtr, EVP_CIPHER_CTX **ctxPtr
) {
    Tcl_Obj      *ivObj = NULL, *keyObj = NULL, *aadObj = NULL, *tagObj = NULL, *inputObj;
    int           result, isBinary = 0, encodingInt = -1;
    const char   *cipherName = "aes-128-gcm";

    Ns_ObjvSpec lopts_encrypt[] = {
        {"-binary",   Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"-aad",      Ns_ObjvObj,    &aadObj,      NULL},
        {"-cipher",   Ns_ObjvString, &cipherName,  NULL},
        {"-encoding", Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-iv",       Ns_ObjvObj,    &ivObj,       NULL},
        {"!-key",     Ns_ObjvObj,    &keyObj,      NULL},
        {"--",        Ns_ObjvBreak,  NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec lopts_decrypt[] = {
        {"-binary",   Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"-aad",      Ns_ObjvObj,    &aadObj,      NULL},
        {"-cipher",   Ns_ObjvString, &cipherName,  NULL},
        {"-encoding", Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-iv",       Ns_ObjvObj,    &ivObj,       NULL},
        {"!-key",     Ns_ObjvObj,    &keyObj,      NULL},
        {"!-tag",     Ns_ObjvObj,    &tagObj,      NULL},
        {"--",        Ns_ObjvBreak,  NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"input", Ns_ObjvObj, &inputObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    *ctxPtr = NULL;

    if (Ns_ParseObjv(encrypt ? lopts_encrypt : lopts_decrypt, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if ((result = GetCipher(interp, cipherName, EVP_CIPH_GCM_MODE, "gcm", cipherPtr)) == TCL_OK) {
        *encodingPtr = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);

        *ctxPtr = EVP_CIPHER_CTX_new();
        *keyStringPtr = Ns_GetBinaryString(keyObj, isBinary == 1, keyLengthPtr, keyDsPtr);

        /*
         * Get optional additional authenticated data (AAD)
         */
        if (aadObj != NULL) {
            *aadStringPtr = Ns_GetBinaryString(aadObj, isBinary == 1, aadLengthPtr, aadDsPtr);
        } else {
            *aadLengthPtr = 0;
            *aadStringPtr = NULL;
        }

        /*
         * Get optional initialization vector (IV)
         */
        if (ivObj != NULL) {
            *ivStringPtr = Ns_GetBinaryString(ivObj, isBinary == 1, ivLengthPtr, ivDsPtr);
        } else {
            *ivStringPtr = NULL;
            *ivLengthPtr = 0;
        }

        if (tagObj != NULL) {
            const char *objType = tagObj->typePtr != NULL ? tagObj->typePtr->name : "NONE";
            TCL_SIZE_T  objLength = tagObj->length;

            *tagStringPtr = (const char *)Ns_GetBinaryString(tagObj, 1 /*isBinary == 1*/, tagLengthPtr, tagDsPtr);
            if (*tagLengthPtr != 16) {
                Ns_Log(Error, "aead: invalid tag length %ld (isBinary %d, objType %s, objLength %ld)\n",
                       (long)*tagLengthPtr, isBinary, objType, (long)objLength);
            }
        } else {
            *tagStringPtr = NULL;
            *tagLengthPtr = 0;
        }

        if (*ivLengthPtr > EVP_MAX_IV_LENGTH
            || (*ivLengthPtr == 0 && EVP_CIPHER_iv_length(*cipherPtr) > 0)
            ) {
            Ns_TclPrintfResult(interp, "initialization vector is invalid (default length for %s: %d bytes)",
                               cipherName, EVP_CIPHER_iv_length(*cipherPtr));
            result = TCL_ERROR;

        } else if (*ctxPtr == NULL) {
            Ns_TclPrintfResult(interp, "could not create encryption context");
            result = TCL_ERROR;

        } else {
            *inputStringPtr = Ns_GetBinaryString(inputObj, NS_TRUE, inputLengthPtr, inputDsPtr);
            result = TCL_OK;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoAeadStringObjCmd -- Subcommand of NsTclCryptoAeadObjCmd
 *
 *        Implements "ns_crypto::aead::encrypt string" and
 *        "ns_crypto::aead::decrypt string". Subcommand to encrypt or
 *        decrypt string data. Encryption returns a dict with "bytes"
 *        and the "tag" necessary for decoding.
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int
CryptoAeadStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, bool encrypt)
{
    int                  result;
    const EVP_CIPHER    *cipher = NULL;
    Tcl_DString          ivDs, keyDs, aadDs, tagDs, inputDs;
    Ns_BinaryEncoding    encoding = NS_OBJ_ENCODING_HEX;
    EVP_CIPHER_CTX      *ctx;
    const unsigned char *inputString = NULL, *ivString = NULL, *aadString = NULL, *keyString = NULL;
    const char          *tagString = NULL;
    TCL_SIZE_T           inputLength, keyLength, ivLength, aadLength, tagLength;

    /*
      ::ns_crypto::aead::encrypt string -cipher aes-128-gcm -iv 123456789 -key secret "hello world"

      set d [::ns_crypto::aead::encrypt string -cipher aes-128-gcm -iv 123456789 -key secret -encoding binary "hello world"]
      ns_crypto::aead::decrypt string -cipher aes-128-gcm -iv 123456789 -key secret -tag [dict get $d tag] -encoding binary [dict get $d bytes]

    */

    Tcl_DStringInit(&inputDs);
    Tcl_DStringInit(&aadDs);
    Tcl_DStringInit(&keyDs);
    Tcl_DStringInit(&ivDs);
    Tcl_DStringInit(&tagDs);

    result = CryptoAeadStringGetArguments(interp, objc, objv, encrypt,
                                          &ivDs, &keyDs, &aadDs, &tagDs, &inputDs,
                                          &keyString,   &keyLength,
                                          &ivString,    &ivLength,
                                          &aadString,   &aadLength,
                                          &tagString,   &tagLength,
                                          &inputString, &inputLength,
                                          &cipher, &encoding, &ctx);
    if (result == TCL_OK) {
        int length;

        if (encrypt) {
            /*
             * Encrypt ...
             */
            /* Init + IV length */
            if ( EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1
                 || !AEAD_Set_ivlen(ctx, (size_t)ivLength)
                 || EVP_EncryptInit_ex(ctx, NULL, NULL, keyString, ivString) != 1
                 ) {
                Ns_TclPrintfResult(interp, "could not initialize encryption context");
                result = TCL_ERROR;

            } else if (aadString != NULL
                       && EVP_EncryptUpdate(ctx, NULL, &length, aadString, (int)aadLength) != 1) {
                /*
                 * To specify additional authenticated data (AAD), a call
                 * to EVP_CipherUpdate(), EVP_EncryptUpdate() or
                 * EVP_DecryptUpdate() should be made with the output
                 * parameter out set to NULL.
                 */
                Ns_TclPrintfResult(interp, "could not set additional authenticated data (AAD)");
                result = TCL_ERROR;

            } else {
                int          cipherBlockSize = EVP_CIPHER_block_size(cipher);
                TCL_SIZE_T   outputLength;
                Tcl_Obj     *listObj;
                Tcl_DString  outputDs;

                Tcl_DStringInit(&outputDs);

                /*
                 * Everything is set up successfully, now do the "real" encryption work.
                 *
                 * Provide the message to be encrypted, and obtain the
                 * encrypted output.  EVP_EncryptUpdate can be called
                 * multiple times if necessary.
                 */
                Tcl_DStringSetLength(&outputDs, inputLength + (TCL_SIZE_T)cipherBlockSize);
                if (EVP_EncryptUpdate(ctx, (unsigned char *)outputDs.string, &length,
                                      inputString, (int)inputLength) == 0) {
                    Ns_TclPrintfResult(interp, "encryption of data failed");
                    result = TCL_ERROR;
                } else {
                    outputLength = (TCL_SIZE_T)length;

                    /*fprintf(stderr, "allocated size %d, inputLength %d cipherBlockSize %d actual size %d\n",
                      (inputLength + cipherBlockSize), inputLength, cipherBlockSize, outputLength);*/
                    assert((inputLength + cipherBlockSize) >= outputLength);

                    if (EVP_EncryptFinal_ex(ctx,
                                            (unsigned char  *)(outputDs.string + length),
                                            &length) == 0) {
                        Ns_TclPrintfResult(interp, "finalization of encryption failed");
                        result = TCL_ERROR;

                    } else {
                        outputLength += (TCL_SIZE_T)length;
                        /*fprintf(stderr, "allocated size %d, final size %d\n", (inputLength + cipherBlockSize), outputLength);*/
                        Tcl_DStringSetLength(&outputDs, outputLength);
                    }
                }
                if (result == TCL_OK) {
                    /*
                     * Get the tag
                     */
                    Tcl_DStringSetLength(&tagDs, 16);

                    if (!AEAD_Get_tag(ctx, (unsigned char*)tagDs.string, (size_t)tagDs.length)) {
                        Ns_TclPrintfResult(interp, "Cannot extract tag string from encrypted data");
                        result = TCL_ERROR;
                    }
                }
                if (result == TCL_OK) {
                    /*
                     * Build result dict.
                     */
                    listObj = Tcl_NewListObj(0, NULL);
                    /*
                     * Convert the result to the output format and return a
                     * dict containing "bytes" and "tag" as the interp result.
                     */
                    Tcl_ListObjAppendElement(interp, listObj, NsAtomObj(NS_ATOM_BYTES));
                    Tcl_ListObjAppendElement(interp, listObj, NsEncodedObj((unsigned char *)outputDs.string,
                                                                         (size_t)outputDs.length,
                                                                         NULL, encoding));
                    Tcl_ListObjAppendElement(interp, listObj, NsAtomObj(NS_ATOM_TAG));
                    Tcl_ListObjAppendElement(interp, listObj, NsEncodedObj((unsigned char *)tagDs.string,
                                                                         (size_t)tagDs.length,
                                                                         NULL, encoding));
                    Tcl_SetObjResult(interp, listObj);
                    Tcl_DStringFree(&outputDs);
                }
            }

        } else {
            /*
             * Decrypt ...
             */
            assert(!encrypt);

            if ( EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1
                        || !AEAD_Set_ivlen(ctx, (size_t)ivLength)
                        || EVP_DecryptInit_ex(ctx, NULL, NULL, keyString, ivString) != 1
                        ) {
                Ns_TclPrintfResult(interp, "could not initialize decryption context");
                result = TCL_ERROR;
            } else if (aadString != NULL
                       && EVP_DecryptUpdate(ctx, NULL, &length, aadString, (int)aadLength) != 1) {
                /*
                 * To specify additional authenticated data (AAD), a call
                 * to EVP_CipherUpdate(), EVP_EncryptUpdate() or
                 * EVP_DecryptUpdate() should be made with the output
                 * parameter out set to NULL.
                 */
                Ns_TclPrintfResult(interp, "could not set additional authenticated data (AAD)");
                result = TCL_ERROR;

            } else if (!AEAD_Set_tag(ctx, (const unsigned char *)tagString, (size_t)tagLength)) {
                Ns_TclPrintfResult(interp,
                                   "could not set authentication tag");
                result = TCL_ERROR;

            } else {
                TCL_SIZE_T   outputLength;
                Tcl_DString  outputDs;

                Tcl_DStringInit(&outputDs);

                /*
                 * Everything is set up successfully, now do the "real" decryption work.
                 *
                 * Provide the input to be decrypted, and obtain the plaintext output.
                 * EVP_DecryptUpdate can be called multiple times if necessary.
                 */
                Tcl_DStringSetLength(&outputDs, inputLength);
                if (EVP_DecryptUpdate(ctx, (unsigned char *)outputDs.string, &length,
                                      inputString, (int)inputLength) == 0) {
                    Ns_TclPrintfResult(interp, "decryption of data failed");
                    result = TCL_ERROR;
                } else {
                    outputLength = (TCL_SIZE_T)length;

                    (void)EVP_DecryptFinal_ex(ctx,
                                              (unsigned char *)(outputDs.string + length),
                                              &length);
                    outputLength += (TCL_SIZE_T)length;
                    //fprintf(stderr, "allocated size %d, final size %d\n", inputLength, outputLength);
                    Tcl_DStringSetLength(&outputDs, outputLength);
                    Tcl_SetObjResult(interp, NsEncodedObj((unsigned char *)outputDs.string,
                                                        (size_t)outputDs.length,
                                                        NULL, encoding));
                }
                Tcl_DStringFree(&outputDs);
            }

        }
        /* Clean up */
        EVP_CIPHER_CTX_free(ctx);
    }

    Tcl_DStringFree(&inputDs);
    Tcl_DStringFree(&aadDs);
    Tcl_DStringFree(&keyDs);
    Tcl_DStringFree(&ivDs);
    Tcl_DStringFree(&tagDs);

    return result;
}

static int
CryptoAeadEncryptStringObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return CryptoAeadStringObjCmd(clientData, interp, objc, objv, NS_TRUE);
}
static int
CryptoAeadDecryptStringObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return CryptoAeadStringObjCmd(clientData, interp, objc, objv, NS_FALSE);
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoAeadEncryptObjCmd, NsTclCryptoAeadDecryptObjCmd --
 *
 *      Implements "ns_crypto::aead::encrypt" and
 *      "ns_crypto::aead::decrypt". Returns encrypted/decrypted data.
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoAeadEncryptObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoAeadEncryptStringObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
int
NsTclCryptoAeadDecryptObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoAeadDecryptStringObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*======================================================================
 * Function Implementations: ns_crypto::key
 *======================================================================
 */

/*----------------------------------------------------------------------
 *
 * SetResultFromRawPublicKey --
 *
 *      Extract raw public key material from an EVP_PKEY and set the
 *      result in the Tcl interpreter.
 *
 *      For EC keys, the public key is returned as an uncompressed point
 *      (0x04 || X || Y). For provider-based keys (e.g., EdDSA, ML-KEM),
 *      the raw public key is obtained via OpenSSL export functions.
 *
 *      When available, legacy raw public key APIs are used as a fallback.
 *
 *      The extracted key material is encoded according to the provided
 *      Ns_BinaryEncoding.
 *
 * Results:
 *      TCL_OK on success.
 *      TCL_ERROR if extraction fails or is not supported.
 *
 * Side effects:
 *      Sets the Tcl interpreter result on success.
 *
 *----------------------------------------------------------------------
 */
static int
SetResultFromRawPublicKey(Tcl_Interp *interp, EVP_PKEY *pkey, Ns_BinaryEncoding encoding)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(pkey != NULL);

# ifndef OPENSSL_NO_EC
#  ifdef HAVE_OPENSSL_3
    if (EVP_PKEY_is_a(pkey, "EC") == 1)
#  else
    if (EVP_PKEY_base_id(pkey) == EVP_PKEY_EC)
#  endif
    {
        EC_KEY         *eckey = EVP_PKEY_get1_EC_KEY(pkey);
        const EC_POINT *ecpoint = NULL;
        int             result;
        Tcl_DString     ds;
        BN_CTX         *bn_ctx = NULL;

        if (eckey == NULL) {
            Ns_TclPrintfResult(interp, "could not obtain EC key");
            return TCL_ERROR;
        }

        ecpoint = EC_KEY_get0_public_key(eckey);
        if (ecpoint == NULL) {
            EC_KEY_free(eckey);
            Ns_TclPrintfResult(interp, "no public EC point available");
            return TCL_ERROR;
        }

        bn_ctx = BN_CTX_new();
        Tcl_DStringInit(&ds);
        SetResultFromEC_POINT(interp, &ds, eckey, ecpoint, bn_ctx, encoding);
        Tcl_DStringFree(&ds);
        BN_CTX_free(bn_ctx);
        EC_KEY_free(eckey);

        result = TCL_OK;
        return result;
    }
# endif

# ifdef HAVE_OPENSSL_3
    {
        unsigned char *pub = NULL;
        size_t         publen = 0u;

        /*
         * For provider-era raw/exportable public keys such as
         * Ed25519/X25519 and ML-KEM.
         */
        publen = EVP_PKEY_get1_encoded_public_key(pkey, &pub);
        if (publen > 0u && pub != NULL) {
            Tcl_SetObjResult(interp, NsEncodedObj(pub, publen, NULL, encoding));
            OPENSSL_free(pub);
            return TCL_OK;
        }
    }
# endif

    /*
     * Fallback for raw public-key APIs available for selected legacy/raw types.
     */
# ifndef HAVE_OPENSSL_PRE_1_1
    {
        unsigned char *buf = NULL;
        size_t         len = 0u;

        if (EVP_PKEY_get_raw_public_key(pkey, NULL, &len) == 1 && len > 0u) {
            buf = ns_malloc(len);
            if (EVP_PKEY_get_raw_public_key(pkey, buf, &len) == 1) {
                Tcl_SetObjResult(interp, NsEncodedObj(buf, len, NULL, encoding));
                ns_free(buf);
                return TCL_OK;
            }
            ns_free(buf);
        }
    }
# endif

    Ns_TclPrintfResult(interp, "raw public key extraction is not supported for this key type");
    return TCL_ERROR;
}

/*----------------------------------------------------------------------
 *
 * SetResultFromRawPrivateKey --
 *
 *      Extract raw private key material from an EVP_PKEY and set the
 *      result in the Tcl interpreter.
 *
 *      This function uses OpenSSL raw key APIs where available. Only
 *      key types supporting raw private key export are handled. For
 *      unsupported key types, an error is returned.
 *
 *      The extracted key material is encoded according to the provided
 *      Ns_BinaryEncoding.
 *
 * Results:
 *      TCL_OK on success.
 *      TCL_ERROR if extraction fails or is not supported.
 *
 * Side effects:
 *      Sets the Tcl interpreter result on success.
 *
 *----------------------------------------------------------------------
 */
static int
SetResultFromRawPrivateKey(Tcl_Interp *interp, EVP_PKEY *pkey, Ns_BinaryEncoding encoding)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(pkey != NULL);

# ifndef HAVE_OPENSSL_PRE_1_1
    {
        unsigned char *buf = NULL;
        size_t         len = 0u;

        if (EVP_PKEY_get_raw_private_key(pkey, NULL, &len) == 1 && len > 0u) {
            buf = ns_malloc(len);
            if (EVP_PKEY_get_raw_private_key(pkey, buf, &len) == 1) {
                Tcl_SetObjResult(interp, NsEncodedObj(buf, len, NULL, encoding));
                ns_free(buf);
                return TCL_OK;
            }
            ns_free(buf);
        }
    }
# endif

    Ns_TclPrintfResult(interp, "raw private key extraction is not supported for this key type");
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * PkeyMatchesPrefix --
 *
 *      Determine whether the type name of the provided EVP_PKEY
 *      matches the specified prefix.
 *
 * Results:
 *      Boolean
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
PkeyMatchesPrefix(EVP_PKEY *pkey, const char *prefix, size_t prefixLength)
{
# ifdef HAVE_OPENSSL_3
    const char *name = EVP_PKEY_get0_type_name(pkey);
    return (name != NULL && strncmp(name, prefix, prefixLength) == 0);
# else
    (void)prefix;
    (void)prefixLength;
    return NS_FALSE;
# endif
}

# ifndef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * PkeyMatchesSubstring --
 *
 *      Determine whether the type name of the provided EVP_PKEY
 *      includes the specified needle string.
 *
 * Results:
 *      Boolean
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
PkeyMatchesSubstring(EVP_PKEY *pkey, const char *needle)
{
#  ifdef HAVE_OPENSSL_3
    const char *name = EVP_PKEY_get0_type_name(pkey);
    return (name != NULL && strstr(name, needle) != NULL);
#  else
    (void)needle;
    return NS_FALSE;
#  endif
}
# endif
/*
 *----------------------------------------------------------------------
 *
 * PkeySupportsSignature --
 *
 *      Determine whether the provided EVP_PKEY supports signing and
 *      verification operations.
 *
 *      This includes classical algorithms (RSA, DSA, EC), modern raw
 *      signature schemes (Ed25519, Ed448), and post-quantum schemes
 *      (e.g., ML-DSA).
 *
 * Results:
 *      NS_TRUE  if the key supports signature operations.
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
PkeySupportsSignature(EVP_PKEY *pkey)
{
# ifdef HAVE_OPENSSL_3
    return (PkeyInstanceCapabilities(pkey) & NS_CRYPTO_CAP_SIGNATURE) != 0u;
# else
    return (PkeyMatchesPrefix(pkey, "ML-DSA-", 7)
            || PkeyMatchesPrefix(pkey, "SLH-DSA-", 8)
            || PkeyIsType(pkey, "RSA", EVP_PKEY_RSA)
            || PkeyIsType(pkey, "DSA", EVP_PKEY_DSA)
            || PkeyIsType(pkey, "EC",  EVP_PKEY_EC)
#  ifdef EVP_PKEY_ED25519
            || PkeyIsType(pkey, "ED25519", EVP_PKEY_ED25519)
#  endif
#  ifdef EVP_PKEY_ED448
            || PkeyIsType(pkey, "ED448", EVP_PKEY_ED448)
#  endif
            );
# endif
}

/*
 *----------------------------------------------------------------------
 *
 * PkeySupportsKem --
 *
 *      Determine whether the provided EVP_PKEY supports key
 *      encapsulation operations.
 *
 *      This includes post-quantum schemes such as ML_KEM.
 *
 * Results:
 *      NS_TRUE  if the key supports key encapsulation operations.
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
PkeySupportsKem(EVP_PKEY *pkey)
{
# ifdef HAVE_OPENSSL_3
    return (PkeyInstanceCapabilities(pkey) & NS_CRYPTO_CAP_KEM) != 0u;
# else
    return (PkeyMatchesPrefix(pkey, "ML-KEM-", 7)
            || PkeyMatchesSubstring(pkey, "MLKEM")
            );
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * PkeySupportsAgreement --
 *
 *      Determine whether the provided EVP_PKEY supports key
 *      agreement operations.
 *
 *      This includes schemes such as DH, X25519 and X448.
 *
 * Results:
 *      NS_TRUE  if the key supports key agreement operations.
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
PkeySupportsAgreement(EVP_PKEY *pkey)
{
# ifdef HAVE_OPENSSL_3
    return (PkeyInstanceCapabilities(pkey) & NS_CRYPTO_CAP_AGREEMENT) != 0u;
# else
    return (PkeyIsType(pkey, "DH", EVP_PKEY_DH)
#  ifdef EVP_PKEY_DHX
            || PkeyIsType(pkey, "DHX", EVP_PKEY_DHX)
#  endif
            || PkeyIsType(pkey, "EC", EVP_PKEY_EC)
            //# ifdef EVP_PKEY_SM2
            //|| PkeyIsType(pkey, "SM2", EVP_PKEY_X448)
            //# endif
#  ifdef EVP_PKEY_X25519
            || PkeyIsType(pkey, "X25519", EVP_PKEY_X25519)
#  endif
#  ifdef EVP_PKEY_X448
            || PkeyIsType(pkey, "X448", EVP_PKEY_X448)
#  endif
            );
# endif
}



/*
 *----------------------------------------------------------------------
 *
 * PkeySignatureRequiresDigest --
 *
 *      Determine whether the provided key type requires an external
 *      message digest (hash function) when performing signature
 *      operations.
 *
 *      Classical algorithms such as RSA, DSA, and ECDSA require an
 *      external digest (e.g., SHA-256), while modern algorithms such
 *      as Ed25519, Ed448, and ML-DSA perform hashing internally and
 *      must be used with a NULL digest.
 *
 * Results:
 *      NS_TRUE  if an external digest must be provided.
 *      NS_FALSE if the algorithm uses an internal digest.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
PkeySignatureSupportsNullDigest(EVP_PKEY *pkey)
{
# ifdef HAVE_OPENSSL_3
    if (PkeyMatchesPrefix(pkey, "ML-DSA-", 7)
        || PkeyMatchesPrefix(pkey, "SLH-DSA-", 8)) {
        return NS_TRUE;
    }
# endif

# ifdef EVP_PKEY_ED25519
    if (PkeyIsType(pkey, "ED25519", EVP_PKEY_ED25519)) {
        return NS_TRUE;
    }
# endif
# ifdef EVP_PKEY_ED448
    if (PkeyIsType(pkey, "ED448", EVP_PKEY_ED448)) {
        return NS_TRUE;
    }
# endif

    return NS_FALSE;
}

static bool
PkeySignatureRequiresDigest(EVP_PKEY *pkey)
{
# ifdef HAVE_OPENSSL_3
    return ((PkeyProbeSignatureCaps(pkey) & NS_CRYPTO_CAP_SIGNATURE) != 0u
            && !PkeySignatureSupportsNullDigest(pkey));
# else
    return (PkeyIsType(pkey, "RSA", EVP_PKEY_RSA)
            || PkeyIsType(pkey, "DSA", EVP_PKEY_DSA)
            || PkeyIsType(pkey, "EC",  EVP_PKEY_EC));
# endif
}


# ifdef HAVE_OPENSSL_3
static unsigned
PkeyProbeSignatureCaps(EVP_PKEY *pkey)
{
    unsigned      caps = NS_CRYPTO_CAP_NONE;
    EVP_MD_CTX   *mctx = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    int           rcNull, rcSha256;

    mctx = EVP_MD_CTX_new();
    if (mctx == NULL) {
        return NS_CRYPTO_CAP_NONE;
    }

    /*
     * Probe 1: NULL digest.
     *
     * For Ed25519/Ed448 this is the required mode.
     */
    rcNull = EVP_DigestSignInit_ex(mctx, &pctx,
                                   NULL, NULL, NULL, pkey, NULL);

    EVP_MD_CTX_free(mctx);
    mctx = EVP_MD_CTX_new();
    pctx = NULL;
    if (mctx == NULL) {
        caps = NS_CRYPTO_CAP_NONE;

    } else {
        /*
         * Probe 2: explicit digest.
         *
         * SHA256 is a reasonable generic probe for digest-based signing.
         */
        rcSha256 = EVP_DigestSignInit_ex(mctx, &pctx,
                                         "SHA256", NULL, NULL, pkey, NULL);

        EVP_MD_CTX_free(mctx);

        if (rcNull > 0 || rcSha256 > 0) {
            caps |= NS_CRYPTO_CAP_SIGNATURE;
        }
    }

    ERR_clear_error();
    return caps;
}

static unsigned
PkeyInstanceCapabilities(EVP_PKEY *pkey)
{
    unsigned      caps = NS_CRYPTO_CAP_NONE;
    EVP_PKEY_CTX *ctx = NULL;

    NS_NONNULL_ASSERT(pkey != NULL);

    ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    if (ctx == NULL) {
        caps = NS_CRYPTO_CAP_NONE;

    } else {

        caps |= NS_CRYPTO_CAP_KEYMGMT;
        EVP_PKEY_CTX_free(ctx);

        /*
         * Signature capability + requires-digest
         */
        caps |= PkeyProbeSignatureCaps(pkey);

        /*
         * Agreement capability
         */
        ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
        if (ctx != NULL) {
            if (EVP_PKEY_derive_init(ctx) > 0) {
                caps |= NS_CRYPTO_CAP_AGREEMENT;
            }
            EVP_PKEY_CTX_free(ctx);
        }

        /*
         * KEM capability
         */
        ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
        if (ctx != NULL) {
            if (EVP_PKEY_encapsulate_init(ctx, NULL) > 0
                || EVP_PKEY_decapsulate_init(ctx, NULL) > 0
                //&& !PkeyIsType(pkey, "EC", EVP_PKEY_EC)
                ) {
                caps |= NS_CRYPTO_CAP_KEM;
            }
            EVP_PKEY_CTX_free(ctx);
        }
    }

    ERR_clear_error();
    return caps;
}
# endif /* HAVE_OPENSSL_3 */

/*----------------------------------------------------------------------
 *
 * PkeyTypeNameObj --
 *
 *      Helper function to obtain the type name of an EVP_PKEY as a
 *      Tcl object.
 *
 *      Under OpenSSL 3, the type name is retrieved via
 *      EVP_PKEY_get0_type_name() and normalized to lowercase.
 *      For older versions, the type name is derived from the legacy
 *      key identifier.
 *
 * Results:
 *      Returns a Tcl_Obj containing the lowercase key type name.
 *      Returns NULL and sets an error message in the interpreter
 *      on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
PkeyTypeNameObj(Tcl_Interp *interp, EVP_PKEY *pkey)
{
#ifdef HAVE_OPENSSL_3
    const char *typeName = EVP_PKEY_get0_type_name(pkey);
    Tcl_DString ds;

    if (typeName == NULL) {
        Ns_TclPrintfResult(interp, "could not determine key type");
        return NULL;
    }
    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, typeName, TCL_INDEX_NONE);
    Ns_StrToLower(ds.string);
    return Ns_DStringToObj(&ds);
#else
    const char *typeName = NULL;

    switch (EVP_PKEY_base_id(pkey)) {
    case EVP_PKEY_RSA:     typeName = "rsa";     break;
    case EVP_PKEY_DSA:     typeName = "dsa";     break;
    case EVP_PKEY_DH:      typeName = "dh";      break;
    case EVP_PKEY_EC:      typeName = "ec";      break;
#ifdef EVP_PKEY_X25519
    case EVP_PKEY_X25519:  typeName = "x25519";  break;
#endif
#ifdef EVP_PKEY_X448
    case EVP_PKEY_X448:    typeName = "x448";    break;
#endif
#ifdef EVP_PKEY_ED25519
    case EVP_PKEY_ED25519: typeName = "ed25519"; break;
#endif
#ifdef EVP_PKEY_ED448
    case EVP_PKEY_ED448:   typeName = "ed448";   break;
#endif
    default:
        Ns_TclPrintfResult(interp, "unsupported or unknown key type");
        return NULL;
    }
    return Tcl_NewStringObj(typeName, TCL_INDEX_NONE);
#endif
}

/*----------------------------------------------------------------------
 *
 * PkeyIsType --
 *
 *      Helper function to test whether a given EVP_PKEY matches a
 *      specified key type.
 *
 *      For OpenSSL 3, EVP_PKEY_is_a() is used with the provided type
 *      name. For older OpenSSL versions, the check falls back to
 *      EVP_PKEY_base_id() using the provided legacy identifier.
 *
 * Results:
 *      Returns NS_TRUE when the key matches the requested type,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
PkeyIsType(EVP_PKEY *pkey, const char *name, int legacyId)
{
#ifdef HAVE_OPENSSL_3
    (void)legacyId;
    return EVP_PKEY_is_a(pkey, name) == 1;
#else
    (void)name;
    return EVP_PKEY_base_id(pkey) == legacyId;
#endif
}


/*======================================================================
 * Function Implementations: ns_crypto::key
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutLegacyDetails --
 *
 *      Populate basic key properties (e.g., type, bits, curve) using
 *      legacy OpenSSL APIs.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      Adds general key metadata to resultObj.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyInfoPutLegacyDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey)
{
    int bits = EVP_PKEY_bits(pkey);

    if (bits > 0) {
        Tcl_DictObjPut(interp, resultObj,
                       NsAtomObj(NS_ATOM_BITS),
                       Tcl_NewIntObj(bits));
    }

    /*
     * Keep current EC curve reporting for 1.1.1 and for legacy keys.
     */
    if (PkeyIsType(pkey, "EC", EVP_PKEY_EC) == 1) {
        EC_KEY         *ec = EVP_PKEY_get1_EC_KEY(pkey);
        const EC_GROUP *group;
        int             nid;
        const char     *curveName = NULL;

        if (ec != NULL) {
            group = EC_KEY_get0_group(ec);
            if (group != NULL) {
                nid = EC_GROUP_get_curve_name(group);
                if (nid != NID_undef) {
                    curveName = OBJ_nid2sn(nid);
                    if (curveName != NULL) {
                        Tcl_DictObjPut(interp, resultObj,
                                       NsAtomObj(NS_ATOM_CURVE),
                                       Tcl_NewStringObj(curveName, TCL_INDEX_NONE));
                    }
                }
            }
            EC_KEY_free(ec);
        }
    }

    return TCL_OK;
}

# ifdef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutProviderDetails --
 *
 *      Add provider-specific metadata (provider name, description,
 *      securityBits, securityCategory) to the result dictionary.
 *
 * Results:
 *      TCL_OK.
 *
 * Side effects:
 *      Queries OpenSSL provider-side attributes and augments resultObj
 *      with additional key properties when available.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyInfoPutProviderDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey)
{
    const OSSL_PROVIDER *provider;
    const char          *providerName;
    const char          *description;
    int                  ibits;

    provider = EVP_PKEY_get0_provider(pkey);
    if (provider != NULL) {
        providerName = OSSL_PROVIDER_get0_name(provider);
        if (providerName != NULL) {
            Tcl_DictObjPut(interp, resultObj,
                           NsAtomObj(NS_ATOM_PROVIDER),
                           Tcl_NewStringObj(providerName, TCL_INDEX_NONE));
        }
    }

    description = EVP_PKEY_get0_description(pkey);
    if (description != NULL) {
        Tcl_DictObjPut(interp, resultObj,
                       NsAtomObj(NS_ATOM_DESCRIPTION),
                       Tcl_NewStringObj(description, TCL_INDEX_NONE));
    }

    /*
     * Bits may already be present via EVP_PKEY_bits().  Keep the legacy value
     * if present; only add if not already set.
     */
    ibits = EVP_PKEY_get_bits(pkey);
    if (ibits > 0) {
        int exists;
        Tcl_Obj *dummyObj = NULL;

        exists = Tcl_DictObjGet(interp, resultObj, NsAtomObj(NS_ATOM_BITS), &dummyObj);
        if (exists == TCL_OK && dummyObj == NULL) {
            Tcl_DictObjPut(interp, resultObj,
                           NsAtomObj(NS_ATOM_BITS),
                           Tcl_NewIntObj(ibits));
        }
    }

    /*
     * Prefer provider-side group/curve name if available and not already set.
     * This is useful for provider-native EC/DH/X25519-style keys.
     */
    {
        Tcl_Obj *dummyObj = NULL;
        size_t   len = 0;

        if (Tcl_DictObjGet(interp, resultObj, NsAtomObj(NS_ATOM_CURVE), &dummyObj) == TCL_OK
            && dummyObj == NULL) {
            (void)EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME,
                                                 NULL, 0, &len);
            if (len > 0) {
                char *buf = ns_malloc(len + 1u);

                if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME,
                                                   buf, len + 1u, &len) == 1) {
                    Tcl_DictObjPut(interp, resultObj,
                                   NsAtomObj(NS_ATOM_CURVE),
                                   Tcl_NewStringObj(buf, TCL_INDEX_NONE));
                }
                ns_free(buf);
            }
        }
    }

    if (EVP_PKEY_get_int_param(pkey, OSSL_PKEY_PARAM_SECURITY_BITS, &ibits) == 1
        && ibits > 0) {
        Tcl_DictObjPut(interp, resultObj,
                       Tcl_NewStringObj("securityBits", TCL_INDEX_NONE),
                       Tcl_NewIntObj(ibits));
    }

#  ifdef OSSL_PKEY_PARAM_SECURITY_CATEGORY
    if (EVP_PKEY_get_int_param(pkey, OSSL_PKEY_PARAM_SECURITY_CATEGORY, &ibits) == 1
        && ibits >= 0) {
        Tcl_DictObjPut(interp, resultObj,
                       Tcl_NewStringObj("securityCategory", TCL_INDEX_NONE),
                       Tcl_NewIntObj(ibits));
    }
#  endif

    return TCL_OK;
}
#endif /* HAVE_OPENSSL_3 */

/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutBnPad --
 *
 *      Store a BIGNUM value under the specified dictionary key after
 *      converting it to a fixed-width, zero-padded big-endian byte
 *      string and applying the requested binary encoding.
 *
 *      This helper is intended for key parameters whose exported form
 *      must have a predictable width, such as EC affine coordinates
 *      used for JWK-style representations. In contrast to minimal-length
 *      integer encodings, the result is always exactly "width" bytes long.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on allocation or conversion failure.
 *      On error, an explanatory message is left in the interpreter.
 *
 * Side effects:
 *      Adds the encoded value to resultObj under "name".
 *      Allocates and frees a temporary buffer.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyInfoPutBnPad(Tcl_Interp *interp, Tcl_Obj *resultObj,
                 const char *name, const BIGNUM *bn, size_t width,
                 Ns_BinaryEncoding encoding)
{
    unsigned char *buf;
    int            result = TCL_ERROR;

    buf = ns_malloc((size_t)width);
    if (buf == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate buffer for %s", name);
        return TCL_ERROR;
    }

    if (BN_bn2binpad(bn, buf, (int)width) != (int)width) {
        ns_free(buf);
        Ns_TclPrintfResult(interp, "could not convert %s", name);
        return TCL_ERROR;
    }

    Tcl_DictObjPut(interp, resultObj,
                   Tcl_NewStringObj(name, TCL_INDEX_NONE),
                   NsEncodedObj(buf, width, NULL, encoding));
    ns_free(buf);

    result = TCL_OK;
    return result;
}

# ifdef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutOctets --
 *
 *      Store a raw octet string under the specified dictionary key after
 *      applying the requested binary encoding.
 *
 *      This helper is intended for public key components that are
 *      naturally represented as raw bytes rather than integer values,
 *      such as the public key value of OKP-style key types.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on allocation failure.
 *      On error, an explanatory message is left in the interpreter.
 *
 * Side effects:
 *      Adds the encoded value to resultObj under "name".
 *      Allocates and frees a temporary copy buffer.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyInfoPutOctets(Tcl_Interp *interp, Tcl_Obj *resultObj,
                  const char *name,
                  const unsigned char *value, size_t valueLen,
                  Ns_BinaryEncoding encoding)
{
    unsigned char *buf;
    int            result = TCL_ERROR;

    buf = ns_malloc(valueLen);
    if (buf == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate buffer for %s", name);
        return TCL_ERROR;
    }

    memcpy(buf, value, valueLen);

    Tcl_DictObjPut(interp, resultObj,
                   Tcl_NewStringObj(name, TCL_INDEX_NONE),
                   NsEncodedObj(buf, valueLen, NULL, encoding));
    ns_free(buf);

    result = TCL_OK;
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutOctetParam --
 *
 *      Query an octet-string parameter from an OpenSSL 3 EVP_PKEY and
 *      store it in the result dictionary under the specified key after
 *      applying the requested binary encoding.
 *
 *      This helper is used for provider-based key types whose public
 *      components are exposed as octet-string parameters, such as the
 *      public value of OKP-style key types.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR if the parameter cannot be queried,
 *      memory allocation fails, or the value cannot be stored.
 *      On error, an explanatory message is left in the interpreter.
 *
 * Side effects:
 *      Adds the encoded parameter value to resultObj under "dictName".
 *      Allocates and frees a temporary buffer.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyInfoPutOctetParam(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey,
                      const char *dictName, const char *paramName,
                      Ns_BinaryEncoding encoding)
{
    unsigned char *buf = NULL;
    size_t         len = 0u;
    int            result = TCL_ERROR;

    if (EVP_PKEY_get_octet_string_param(pkey, paramName,
                                        NULL, 0, &len) != 1) {
        Ns_TclPrintfResult(interp, "could not obtain %s length", dictName);
        goto done;
    }

    buf = ns_malloc(len);
    if (buf == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate buffer for %s", dictName);
        goto done;
    }

    if (EVP_PKEY_get_octet_string_param(pkey, paramName,
                                        buf, len, &len) != 1) {
        Ns_TclPrintfResult(interp, "could not obtain %s value", dictName);
        goto done;
    }

    if (PkeyInfoPutOctets(interp, resultObj, dictName, buf, len, encoding) != TCL_OK) {
        goto done;
    }

    result = TCL_OK;

done:
    if (buf != NULL) {
        ns_free(buf);
    }
    return result;
}

# endif /* HAVE_OPENSSL_3 */


/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutOkpDetails --
 *
 *      Add OKP-specific public key details to the dictionary returned by
 *      "ns_crypto::key info".
 *
 *      For supported OpenSSL 3 key types such as Ed25519, Ed448,
 *      X25519, and X448, this function exports the raw public key value
 *      under the key "x" using the requested binary encoding. This
 *      matches the JWK representation of OKP public keys.
 *
 *      For key types outside this family, the function performs no
 *      action and succeeds. On older OpenSSL versions where these key
 *      types are not handled through provider parameters, this function
 *      may be a no-op.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR if the public key value cannot be
 *      queried or stored. On error, an explanatory message is left in
 *      the interpreter.
 *
 * Side effects:
 *      For supported OKP keys, adds the key "x" to resultObj.
 *      May allocate and free temporary buffers.
 *
 *----------------------------------------------------------------------
 */
# ifdef HAVE_OPENSSL_3
static int
PkeyInfoPutOkpDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey,
                      Ns_BinaryEncoding encoding)
{
    if (PkeyIsType(pkey, "ED25519", EVP_PKEY_ED25519)
        || PkeyIsType(pkey, "ED448", EVP_PKEY_ED448)
        || PkeyIsType(pkey, "X25519", EVP_PKEY_X25519)
        || PkeyIsType(pkey, "X448", EVP_PKEY_X448)) {

        if (PkeyInfoPutOctetParam(interp, resultObj, pkey,
                                  "x", OSSL_PKEY_PARAM_PUB_KEY,
                                  encoding) != TCL_OK) {
            return TCL_ERROR;
        }
    }

    return TCL_OK;
}
# else
/* legacy implementation */
static int
PkeyInfoPutOkpDetails(Tcl_Interp * UNUSED(interp), Tcl_Obj * UNUSED(resultObj), EVP_PKEY * UNUSED(pkey),
                      Ns_BinaryEncoding UNUSED(encoding))
{
    return TCL_OK;
}
# endif /* HAVE_OPENSSL_3 */

/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutEcDetails --
 *
 *      Add EC-specific public key details to the dictionary returned by
 *      "ns_crypto::key info".
 *
 *      For EC keys, this function exports the affine public coordinates
 *      "x" and "y" using the requested binary encoding. The coordinates
 *      are emitted as fixed-width, zero-padded big-endian byte strings
 *      whose lengths are determined by the curve. This makes the values
 *      suitable for JWK-style use and consistent across supported
 *      OpenSSL versions.
 *
 *      For non-EC keys, the function performs no action and succeeds.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on unsupported curves, allocation
 *      failures, provider/query failures, or coordinate conversion
 *      errors. On error, an explanatory message is left in the
 *      interpreter.
 *
 * Side effects:
 *      For EC keys, adds the keys "x" and "y" to resultObj.
 *      Allocates and frees temporary OpenSSL and buffer objects.
 *
 *----------------------------------------------------------------------
 */
# ifdef HAVE_OPENSSL_3
static int
PkeyInfoPutEcDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey,
                     Ns_BinaryEncoding encoding)
{
    char    groupName[80];
    size_t  groupNameLen = 0u;
    BIGNUM *x = NULL, *y = NULL;
    size_t  coordLen;
    int     result = TCL_ERROR;

    if (!PkeyIsType(pkey, "EC", EVP_PKEY_EC)) {
        return TCL_OK;
    }

    if (EVP_PKEY_get_utf8_string_param(pkey,
                                       OSSL_PKEY_PARAM_GROUP_NAME,
                                       groupName, sizeof(groupName),
                                       &groupNameLen) != 1) {
        Ns_TclPrintfResult(interp, "could not obtain EC group name");
        goto done;
    }

    if (EcGroupCoordinateLength(groupName, &coordLen) != TCL_OK) {
        Ns_TclPrintfResult(interp, "unsupported EC group \"%s\"", groupName);
        goto done;
    }

    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_X, &x) != 1) {
        Ns_TclPrintfResult(interp, "could not obtain EC x coordinate");
        goto done;
    }
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_Y, &y) != 1) {
        Ns_TclPrintfResult(interp, "could not obtain EC y coordinate");
        goto done;
    }

    if (PkeyInfoPutBnPad(interp, resultObj, "x", x, coordLen, encoding) != TCL_OK) {
        goto done;
    }
    if (PkeyInfoPutBnPad(interp, resultObj, "y", y, coordLen, encoding) != TCL_OK) {
        goto done;
    }

    result = TCL_OK;

done:
    if (x != NULL) {
        BN_free(x);
    }
    if (y != NULL) {
        BN_free(y);
    }
    return result;
}
# else
/* legacy implementation */
static int
PkeyInfoPutEcDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey,
                     Ns_BinaryEncoding encoding)
{
    EC_KEY         *ec = NULL;
    const EC_GROUP *group;
    const EC_POINT *point;
    BIGNUM         *x = NULL, *y = NULL;
    int             nid, result = TCL_ERROR;
    size_t          coordLen;
    const char     *groupName = NULL;

    if (EVP_PKEY_base_id(pkey) != EVP_PKEY_EC) {
        return TCL_OK;
    }

    ec = EVP_PKEY_get1_EC_KEY(pkey);
    if (ec == NULL) {
        Ns_TclPrintfResult(interp, "could not obtain EC key");
        goto done;
    }

    group = EC_KEY_get0_group(ec);
    point = EC_KEY_get0_public_key(ec);
    if (group == NULL || point == NULL) {
        Ns_TclPrintfResult(interp, "EC key does not contain group/public point");
        goto done;
    }

    nid = EC_GROUP_get_curve_name(group);
    if (nid != NID_undef) {
        groupName = OBJ_nid2sn(nid);
    }
    if (groupName == NULL) {
        Ns_TclPrintfResult(interp, "could not determine EC group name");
        goto done;
    }

    if (EcGroupCoordinateLength(groupName, &coordLen) != TCL_OK) {
        Ns_TclPrintfResult(interp, "unsupported EC group \"%s\"", groupName);
        goto done;
    }

    x = BN_new();
    y = BN_new();
    if (x == NULL || y == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate EC coordinate bignums");
        goto done;
    }

# if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (EC_POINT_get_affine_coordinates(group, point, x, y, NULL) != 1) {
        Ns_TclPrintfResult(interp, "could not obtain EC affine coordinates");
        goto done;
    }
# else
    if (EC_POINT_get_affine_coordinates_GFp(group, point, x, y, NULL) != 1) {
        Ns_TclPrintfResult(interp, "could not obtain EC affine coordinates");
        goto done;
    }
# endif

    if (PkeyInfoPutBnPad(interp, resultObj, "x", x, coordLen, encoding) != TCL_OK) {
        goto done;
    }
    if (PkeyInfoPutBnPad(interp, resultObj, "y", y, coordLen, encoding) != TCL_OK) {
        goto done;
    }

    result = TCL_OK;

done:
    if (x != NULL) {
        BN_free(x);
    }
    if (y != NULL) {
        BN_free(y);
    }
    if (ec != NULL) {
        EC_KEY_free(ec);
    }
    return result;
}
# endif /* HAVE_OPENSSL_3 */

# ifdef HAVE_OPENSSL_3
static int
PkeyInfoPutRsaDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey, Ns_BinaryEncoding  encoding)
{
    BIGNUM *n = NULL, *e = NULL;
    int result = TCL_ERROR;

    if (!PkeyIsType(pkey, "RSA", EVP_PKEY_RSA)
        && !PkeyIsType(pkey, "RSA-PSS", EVP_PKEY_RSA_PSS)) {
        return TCL_OK;
    }

    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) != 1) {
        goto done;
    }
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) != 1) {
        goto done;
    }

    if (n != NULL) {
        int nLen = BN_num_bytes(n);
        unsigned char *buf = ns_malloc((size_t)nLen);

        if (buf == NULL) {
            Ns_TclPrintfResult(interp, "could not allocate RSA modulus buffer");
            goto done;
        }
        if (BN_bn2bin(n, buf) != nLen) {
            ns_free(buf);
            Ns_TclPrintfResult(interp, "could not convert RSA modulus");
            goto done;
        }

        Tcl_DictObjPut(interp, resultObj,
                       Tcl_NewStringObj("n", TCL_INDEX_NONE),
                       NsEncodedObj(buf, (size_t)nLen, NULL, encoding));
        ns_free(buf);
    }

    if (e != NULL) {
        int            eLen = BN_num_bytes(e);
        unsigned char *buf = ns_malloc((size_t)eLen);

        if (buf == NULL) {
            Ns_TclPrintfResult(interp, "could not allocate RSA exponent buffer");
            goto done;
        }
        if (BN_bn2bin(e, buf) != eLen) {
            ns_free(buf);
            Ns_TclPrintfResult(interp, "could not convert RSA exponent");
            goto done;
        }

        Tcl_DictObjPut(interp, resultObj,
                       Tcl_NewStringObj("e", TCL_INDEX_NONE),
                       NsEncodedObj(buf, (size_t)eLen, NULL, encoding));
        ns_free(buf);
    }

    result = TCL_OK;

done:
    if (n != NULL) {
        BN_free(n);
    }
    if (e != NULL) {
        BN_free(e);
    }
    return result;
}

#else
/* legacy implementation */
static int
PkeyInfoPutRsaDetails(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey, Ns_BinaryEncoding  encoding)
{
    RSA *rsa = NULL;
    const BIGNUM *n = NULL, *e = NULL;

    if (!PkeyIsType(pkey, "RSA", EVP_PKEY_RSA)
#ifdef EVP_PKEY_RSA_PSS
        && !PkeyIsType(pkey, "RSA-PSS", EVP_PKEY_RSA_PSS)
#endif
        ) {
        return TCL_OK;
    }

    rsa = EVP_PKEY_get1_RSA(pkey);
    if (rsa == NULL) {
        return TCL_OK;
    }

    RSA_get0_key(rsa, &n, &e, NULL);

    if (n != NULL) {
        int nLen = BN_num_bytes(n);
        unsigned char *buf = ns_malloc((size_t)nLen);

        if (buf == NULL) {
            RSA_free(rsa);
            Ns_TclPrintfResult(interp, "could not allocate RSA modulus buffer");
            return TCL_ERROR;
        }
        if (BN_bn2bin(n, buf) != nLen) {
            ns_free(buf);
            RSA_free(rsa);
            Ns_TclPrintfResult(interp, "could not convert RSA modulus");
            return TCL_ERROR;
        }

        Tcl_DictObjPut(interp, resultObj,
                       Tcl_NewStringObj("n", TCL_INDEX_NONE),
                       NsEncodedObj(buf, (size_t)nLen, NULL, encoding));
        ns_free(buf);
    }

    if (e != NULL) {
        int eLen = BN_num_bytes(e);
        unsigned char *buf = ns_malloc((size_t)eLen);

        if (buf == NULL) {
            RSA_free(rsa);
            Ns_TclPrintfResult(interp, "could not allocate RSA exponent buffer");
            return TCL_ERROR;
        }
        if (BN_bn2bin(e, buf) != eLen) {
            ns_free(buf);
            RSA_free(rsa);
            Ns_TclPrintfResult(interp, "could not convert RSA exponent");
            return TCL_ERROR;
        }

        Tcl_DictObjPut(interp, resultObj,
                       Tcl_NewStringObj("e", TCL_INDEX_NONE),
                       NsEncodedObj(buf, (size_t)eLen, NULL, encoding));
        ns_free(buf);
    }

    RSA_free(rsa);
    return TCL_OK;
}
# endif /* HAVE_OPENSSL_3 */

/*
 *----------------------------------------------------------------------
 *
 * PkeyInfoPutCapabilities --
 *
 *      Populate capability flags (signature, agreement, kem,
 *      requiresdigest) for a key into the result dictionary.
 *
 * Results:
 *      TCL_OK.
 *
 * Side effects:
 *      Adds entries to resultObj describing supported operations of
 *      the provided EVP_PKEY.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyInfoPutCapabilities(Tcl_Interp *interp, Tcl_Obj *resultObj, EVP_PKEY *pkey)
{
    int supportsSignature;
    int supportsAgreement;
    int supportsKEM;

    supportsSignature = PkeySupportsSignature(pkey);
    Tcl_DictObjPut(interp, resultObj,
                   NsAtomObj(NS_ATOM_SIGNATURE),
                   Tcl_NewBooleanObj(supportsSignature));
    if (supportsSignature) {
        int requiresDigest = PkeySignatureRequiresDigest(pkey);

        Tcl_DictObjPut(interp, resultObj,
                       NsAtomObj(NS_ATOM_REQUIRESDIGEST),
                       Tcl_NewBooleanObj(requiresDigest));
    }

    supportsAgreement = PkeySupportsAgreement(pkey);
    Tcl_DictObjPut(interp, resultObj,
                   NsAtomObj(NS_ATOM_AGREEMENT),
                   Tcl_NewBooleanObj(supportsAgreement));

    supportsKEM = PkeySupportsKem(pkey);
    Tcl_DictObjPut(interp, resultObj,
                   NsAtomObj(NS_ATOM_KEM),
                   Tcl_NewBooleanObj(supportsKEM));

    return TCL_OK;
}

# ifdef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * CryptoKeyGenerateObjCmd --
 *
 *      Implements "ns_crypto::key generate". Generates a new private
 *      key for the specified algorithm.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      Creates a new key via OpenSSL and returns it in PEM format or
 *      writes it to a file.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKeyGenerateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                        TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const char *typeName = NULL, *outfileName = NULL, *groupName = NULL;
    OSSL_PARAM  params[2];
    OSSL_PARAM *paramPtr = NULL;
    Ns_ObjvSpec lopts[] = {
        {"!-name",   Ns_ObjvString, &typeName,    NULL},
        {"-group",   Ns_ObjvString, &groupName,   NULL},
        {"-outfile", Ns_ObjvString, &outfileName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (KeygenGroupParams(interp, typeName, groupName,
                          "key", params, &paramPtr) != TCL_OK) {
        return TCL_ERROR;
    }

    return GeneratePrivateKeyPem(interp,
                                 typeName,
                                 "key",
                                 outfileName,
                                 NS_CRYPTO_KEYGEN_USAGE_ANY,
                                 paramPtr);
}

#  ifndef OPENSSL_NO_EC
/*
 *----------------------------------------------------------------------
 *
 * PkeyImportEcPublicParamsFromDict --
 *
 *      Extract EC public key parameters from a Tcl dictionary and add
 *      them to an OpenSSL parameter builder.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on invalid input or failure.
 *
 * Side effects:
 *      Allocates temporary buffers for the encoded EC point and records
 *      them in tmpData for later cleanup. Appends parameters to bld.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyImportEcPublicParamsFromDict(Tcl_Interp *interp,
                                 Tcl_Obj *paramsObj,
                                 OSSL_PARAM_BLD *bld,
                                 Ns_DList *tmpData)
{
    Tcl_Obj             *groupObj = NULL, *xObj = NULL, *yObj = NULL;
    const char          *groupName;
    const unsigned char *x, *y;
    unsigned char       *pub = NULL;
    TCL_SIZE_T           groupLen, xLen, yLen;
    size_t               pubLen, expectedLen;
    int                  result = TCL_ERROR;

    if (Tcl_DictObjGet(interp, paramsObj,
                       Tcl_NewStringObj("group", TCL_INDEX_NONE),
                       &groupObj) != TCL_OK
        || Tcl_DictObjGet(interp, paramsObj,
                          Tcl_NewStringObj("x", TCL_INDEX_NONE),
                          &xObj) != TCL_OK
        || Tcl_DictObjGet(interp, paramsObj,
                          Tcl_NewStringObj("y", TCL_INDEX_NONE),
                          &yObj) != TCL_OK) {
        return TCL_ERROR;
    }

    if (groupObj == NULL || xObj == NULL || yObj == NULL) {
        Ns_TclPrintfResult(interp,
                           "EC public key import requires dict keys \"group\", \"x\", and \"y\"");
        return TCL_ERROR;
    }

    groupName = Tcl_GetStringFromObj(groupObj, &groupLen);
    if (groupLen == 0) {
        Ns_TclPrintfResult(interp, "EC public key import requires non-empty \"group\"");
        return TCL_ERROR;
    }

    x = (unsigned char *)Tcl_GetByteArrayFromObj(xObj, &xLen);
    y = (unsigned char *)Tcl_GetByteArrayFromObj(yObj, &yLen);

    if (xLen <= 0 || yLen <= 0 || xLen != yLen) {
        Ns_TclPrintfResult(interp,
                           "EC public key coordinates \"x\" and \"y\" must be non-empty and have equal length");
        goto done;
    }
    if (EcGroupCoordinateLength(groupName, &expectedLen) == TCL_OK) {
        if ((size_t)xLen != expectedLen || (size_t)yLen != expectedLen) {
            Ns_TclPrintfResult(interp,
                               "invalid coordinate length for %s (need %zu bytes each)",
                               groupName, expectedLen);
            goto done;
        }
    }

    /*
     * SEC1 uncompressed point: 0x04 || X || Y
     */
    pubLen = 1u + (size_t)xLen + (size_t)yLen;
    pub = ns_malloc(pubLen);
    if (pub == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate EC public key buffer");
        goto done;
    }
    if (FreelistAdd(interp, tmpData, pub, ns_free) != TCL_OK) {
        goto done;
    }

    pub[0] = 0x04;
    memcpy(pub + 1u, x, (size_t)xLen);
    memcpy(pub + 1u + (size_t)xLen, y, (size_t)yLen);

    if (OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                        groupName, 0) != 1) {
        SetResultFromOsslError(interp, "could not add EC group parameter");
        goto done;
    }

    if (OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                         pub, pubLen) != 1) {
        SetResultFromOsslError(interp, "could not add EC public key parameter");
        goto done;
    }

    result = TCL_OK;

done:
    return result;
}
#  endif /* OPENSSL_NO_EC */

/*
 *----------------------------------------------------------------------
 *
 * PkeyImportRsaPublicParamsFromDict --
 *
 *      Extract RSA public key parameters (modulus n and exponent e)
 *      from a Tcl dictionary and add them to an OpenSSL parameter
 *      builder.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on invalid input or failure.
 *
 * Side effects:
 *      Allocates BIGNUM objects and registers them in tmpData for later
 *      cleanup. Appends parameters to bld.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyImportRsaPublicParamsFromDict(Tcl_Interp *interp,
                                  Tcl_Obj *paramsObj,
                                  OSSL_PARAM_BLD *bld,
                                  Ns_DList *tmpData)
{
    Tcl_Obj             *nObj = NULL, *eObj = NULL;
    const unsigned char *nBytes, *eBytes;
    TCL_SIZE_T           nLen, eLen;
    BIGNUM              *nBn = NULL, *eBn = NULL;
    int                  result = TCL_ERROR;

    if (Tcl_DictObjGet(interp, paramsObj,
                       Tcl_NewStringObj("n", TCL_INDEX_NONE),
                       &nObj) != TCL_OK
        || Tcl_DictObjGet(interp, paramsObj,
                          Tcl_NewStringObj("e", TCL_INDEX_NONE),
                          &eObj) != TCL_OK) {
        return TCL_ERROR;
    }

    if (nObj == NULL || eObj == NULL) {
        Ns_TclPrintfResult(interp,
                           "RSA public key import requires dict keys \"n\" and \"e\"");
        return TCL_ERROR;
    }

    nBytes = (unsigned char *)Tcl_GetByteArrayFromObj(nObj, &nLen);
    eBytes = (unsigned char *)Tcl_GetByteArrayFromObj(eObj, &eLen);

    if (nLen <= 0 || eLen <= 0) {
        Ns_TclPrintfResult(interp,
                           "RSA public key parameters \"n\" and \"e\" must be non-empty");
        goto done;
    }

    nBn = BN_bin2bn(nBytes, (int)nLen, NULL);
    eBn = BN_bin2bn(eBytes, (int)eLen, NULL);
    if (nBn == NULL || eBn == NULL) {
        SetResultFromOsslError(interp, "could not convert RSA parameters");
        goto done;
    }
    if (FreelistAdd(interp, tmpData, nBn, (Ns_FreeProc*)BN_free) != TCL_OK) {
        goto done;
    }
    if (FreelistAdd(interp, tmpData, eBn, (Ns_FreeProc*)BN_free) != TCL_OK) {
        goto done;
    }

    if (OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, nBn) != 1) {
        SetResultFromOsslError(interp, "could not add RSA modulus parameter");
        goto done;
    }

    if (OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, eBn) != 1) {
        SetResultFromOsslError(interp, "could not add RSA exponent parameter");
        goto done;
    }

    result = TCL_OK;

done:
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyImportFromParams --
 *
 *      Construct an EVP_PKEY from OpenSSL parameters and return or
 *      serialize the resulting key.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      Creates an EVP_PKEY via EVP_PKEY_fromdata() and sets the Tcl
 *      result to either a PEM encoding or raw public key bytes.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyImportFromParams(Tcl_Interp *interp,
                     const char *typeName,
                     Ns_CryptoKeyImportSelection selection,
                     OSSL_PARAM *params,
                     int formatInt,
                     Ns_BinaryEncoding encoding,
                     const char *outfileName)
{
    int result = TCL_ERROR;
    EVP_PKEY_CTX *ctx = NULL;
    EVP_PKEY *pkey = NULL;
    int selectionInt;

    switch (selection) {
    case NS_CRYPTO_KEYIMPORT_PUBLIC:
        selectionInt = EVP_PKEY_PUBLIC_KEY;
        break;
    case NS_CRYPTO_KEYIMPORT_PRIVATE:
        selectionInt = EVP_PKEY_PRIVATE_KEY;
        break;
    case NS_CRYPTO_KEYIMPORT_KEYPAIR:
        selectionInt = EVP_PKEY_KEYPAIR;
        break;
    default:
        Ns_TclPrintfResult(interp, "invalid key import selection");
        return TCL_ERROR;
    }

    ERR_clear_error();

    ctx = EVP_PKEY_CTX_new_from_name(NULL, typeName, NULL);
    if (ctx == NULL) {
        SetResultFromOsslError(interp, "could not create key import context");
        goto done;
    }

    if (EVP_PKEY_fromdata_init(ctx) <= 0) {
        SetResultFromOsslError(interp, "could not initialize key import");
        goto done;
    }

    for (const OSSL_PARAM *p = params; p != NULL && p->key != NULL; p++) {
        Ns_Log(Debug, "import param key <%s> type %u size %zu",
               p->key, p->data_type, p->data_size);
    }

    if (EVP_PKEY_fromdata(ctx, &pkey, selectionInt, params) <= 0) {
        SetResultFromOsslError(interp, "could not import key");
        goto done;
    }

    if (formatInt == OUTPUT_FORMAT_PEM) {
        result = PkeyPublicPemWrite(interp, pkey, "key", "public", outfileName);
    } else {
        if (outfileName != NULL) {
            Ns_TclPrintfResult(interp,
                               "the option \"-outfile\" requires \"-format pem\"");
            goto done;
        }
        result = SetResultFromRawPublicKey(interp, pkey, encoding);
    }

done:
    if (pkey != NULL) {
        EVP_PKEY_free(pkey);
    }
    if (ctx != NULL) {
        EVP_PKEY_CTX_free(ctx);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * OkpCurveInfo --
 *
 *      Map a JWK OKP curve name ("crv") to the corresponding OpenSSL
 *      key type name and expected public key length.
 *
 * Results:
 *      TCL_OK       - on success, *typeNamePtr and *pubLenPtr are set
 *      TCL_CONTINUE - if the provided curve name is not supported
 *
 * Side effects:
 *      None.
 *
 * Description:
 *      This function translates the JWK "crv" parameter used for OKP
 *      (Octet Key Pair) keys into the OpenSSL key type name required
 *      for EVP_PKEY_CTX_new_from_name() and validates the expected
 *      size of the public key.
 *
 *      Supported mappings include:
 *
 *          Ed25519 -> type "ED25519", public key length 32 bytes
 *          Ed448   -> type "ED448",   public key length 57 bytes
 *          X25519  -> type "X25519",  public key length 32 bytes
 *          X448    -> type "X448",    public key length 56 bytes
 *
 *      The returned type name is later used as the resolved key type
 *      for EVP_PKEY_fromdata(), while the length is used to validate
 *      the "x" parameter provided by the caller.
 *
 *----------------------------------------------------------------------
 */
static int
OkpCurveInfo(const char *crv, const char **typeNamePtr, size_t *pubLenPtr)
{
    if (STRIEQ(crv, "Ed25519")) {
        *typeNamePtr = "ED25519";
        *pubLenPtr = 32u;
        return TCL_OK;
    }
    if (STRIEQ(crv, "Ed448")) {
        *typeNamePtr = "ED448";
        *pubLenPtr = 57u;
        return TCL_OK;
    }
    if (STRIEQ(crv, "X25519")) {
        *typeNamePtr = "X25519";
        *pubLenPtr = 32u;
        return TCL_OK;
    }
    if (STRIEQ(crv, "X448")) {
        *typeNamePtr = "X448";
        *pubLenPtr = 56u;
        return TCL_OK;
    }
    return TCL_CONTINUE;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyImportOkpPublicParamsFromDict --
 *
 *      Extract OKP public key parameters from a Tcl dictionary and
 *      populate an OpenSSL parameter builder.
 *
 * Results:
 *      TCL_OK    - on success
 *      TCL_ERROR - on invalid input or failure (error message set)
 *
 * Side effects:
 *      Appends parameters to the provided OSSL_PARAM_BLD and sets
 *      *resolvedTypeNamePtr to the OpenSSL key type name.
 *      Allocates temporary buffers recorded in tmpData for cleanup.
 *
 * Description:
 *      This function implements the parameter extraction for
 *      "ns_crypto::key import -name OKP" for public keys.
 *
 *      The input dictionary must contain:
 *
 *          crv - JWK curve name (e.g., "Ed25519", "X25519")
 *          x   - raw public key bytes
 *
 *      The function performs the following steps:
 *
 *        - Validates presence and non-emptiness of "crv" and "x"
 *        - Maps "crv" to an OpenSSL key type via OkpCurveInfo()
 *        - Verifies that the provided public key length matches the
 *          expected size for the curve
 *        - Adds the public key bytes as OSSL_PKEY_PARAM_PUB_KEY to
 *          the parameter builder
 *        - Sets *resolvedTypeNamePtr to the resolved OpenSSL type
 *          name (e.g., "ED25519", "X25519")
 *
 *      The resulting parameters are later consumed by
 *      EVP_PKEY_fromdata() to construct the key.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyImportOkpPublicParamsFromDict(Tcl_Interp *interp,
                                  Tcl_Obj *paramsObj,
                                  const char **resolvedTypeNamePtr,
                                  OSSL_PARAM_BLD *bld,
                                  Ns_DList *tmpData)
{
    Tcl_Obj             *crvObj = NULL, *xObj = NULL;
    const char          *crv, *typeName = NULL;
    const unsigned char *xBytes;
    TCL_SIZE_T           crvLen, xLen;
    size_t               expectedLen;
    unsigned char       *pub = NULL;
    int                  result = TCL_ERROR;

    if (Tcl_DictObjGet(interp, paramsObj,
                       Tcl_NewStringObj("crv", TCL_INDEX_NONE),
                       &crvObj) != TCL_OK
        || Tcl_DictObjGet(interp, paramsObj,
                          Tcl_NewStringObj("x", TCL_INDEX_NONE),
                          &xObj) != TCL_OK) {
        return TCL_ERROR;
    }

    if (crvObj == NULL || xObj == NULL) {
        Ns_TclPrintfResult(interp,
                           "OKP public key import requires dict keys \"crv\" and \"x\"");
        return TCL_ERROR;
    }

    crv = Tcl_GetStringFromObj(crvObj, &crvLen);
    if (crvLen == 0) {
        Ns_TclPrintfResult(interp, "OKP public key import requires non-empty \"crv\"");
        return TCL_ERROR;
    }

    xBytes = (const unsigned char *)Tcl_GetByteArrayFromObj(xObj, &xLen);
    if (xLen <= 0) {
        Ns_TclPrintfResult(interp,
                           "OKP public key parameter \"x\" must be non-empty");
        return TCL_ERROR;
    }

    if (OkpCurveInfo(crv, &typeName, &expectedLen) != TCL_OK) {
        Ns_TclPrintfResult(interp, "unsupported OKP curve \"%s\"", crv);
        return TCL_ERROR;
    }

    if ((size_t)xLen != expectedLen) {
        Ns_TclPrintfResult(interp,
                           "invalid public key length for %s (need %zu bytes)",
                           crv, expectedLen);
        return TCL_ERROR;
    }

    pub = ns_malloc((size_t)xLen);
    if (pub == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate OKP public key buffer");
        return TCL_ERROR;
    }
    memcpy(pub, xBytes, (size_t)xLen);

    if (FreelistAdd(interp, tmpData, pub, ns_free) != TCL_OK) {
        return TCL_ERROR;
    }

    if (OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                         pub, (size_t)xLen) != 1) {
        SetResultFromOsslError(interp, "could not add OKP public key parameter");
        return TCL_ERROR;
    }

    *resolvedTypeNamePtr = typeName;
    result = TCL_OK;
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeyImportParamsFromDict --
 *
 *      Dispatch parameter extraction for key import based on algorithm
 *      type and selection.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR if the algorithm is unsupported or
 *      parameters are invalid.
 *
 * Side effects:
 *      Appends parameters to the provided OpenSSL builder and records
 *      temporary allocations in tmpData.
 *
 *----------------------------------------------------------------------
 */
static int
PkeyImportParamsFromDict(Tcl_Interp *interp,
                         const char *typeName,
                         Ns_CryptoKeyImportSelection selection,
                         Tcl_Obj *paramsObj,
                         const char **resolvedTypeNamePtr,
                         OSSL_PARAM_BLD *bld,
                         Ns_DList *tmpData)
{
    *resolvedTypeNamePtr = NULL;

#  ifndef OPENSSL_NO_EC
    if (STRIEQ(typeName, "EC")) {
        if (selection != NS_CRYPTO_KEYIMPORT_PUBLIC) {
            Ns_TclPrintfResult(interp,
                               "key import for algorithm \"%s\" currently supports only -from public",
                               typeName);
            return TCL_ERROR;
        }
        *resolvedTypeNamePtr = "EC";
        return PkeyImportEcPublicParamsFromDict(interp, paramsObj, bld, tmpData);
    }
#  endif

    if (STRIEQ(typeName, "RSA")) {
        if (selection != NS_CRYPTO_KEYIMPORT_PUBLIC) {
            Ns_TclPrintfResult(interp,
                               "key import for algorithm \"%s\" currently supports only -from public",
                               typeName);
            return TCL_ERROR;
        }
        *resolvedTypeNamePtr = "RSA";
        return PkeyImportRsaPublicParamsFromDict(interp, paramsObj, bld, tmpData);
    }

    if (STRIEQ(typeName, "OKP")) {
        if (selection != NS_CRYPTO_KEYIMPORT_PUBLIC) {
            Ns_TclPrintfResult(interp,
                               "key import for algorithm \"%s\" currently supports only -from public",
                               typeName);
            return TCL_ERROR;
        }
        return PkeyImportOkpPublicParamsFromDict(interp, paramsObj,
                                                 resolvedTypeNamePtr,
                                                 bld, tmpData);
    }

    Ns_TclPrintfResult(interp,
                       "key import for algorithm \"%s\" is not yet implemented",
                       typeName);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoKeyImportObjCmd --
 *
 *      Implements "ns_crypto::key import". Constructs a key from
 *      algorithm-specific parameters provided as a Tcl dictionary.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on invalid input or failure.
 *
 * Side effects:
 *      Builds OpenSSL parameter structures, creates an EVP_PKEY, and
 *      returns it in the requested format. Temporary objects are
 *      tracked and freed via a cleanup list.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKeyImportObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                      TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int             formatInt = OUTPUT_FORMAT_PEM, encodingInt = -1;
    int             fromInt = NS_CRYPTO_KEYIMPORT_PUBLIC;
    const char     *typeName = NULL, *outfileName = NULL, *resolvedTypeName = NULL;
    Tcl_Obj        *paramsObj = NULL;
    OSSL_PARAM_BLD *bld = NULL;
    OSSL_PARAM     *params = NULL;
    int             result = TCL_ERROR;
    Ns_DList        tmpData;
    TCL_SIZE_T      dictSize = 0;
    Ns_BinaryEncoding           encoding;
    Ns_CryptoKeyImportSelection selection;
    static Ns_ObjvTable keyImportFormats[] = {
        {"raw", OUTPUT_FORMAT_RAW},
        {"pem", OUTPUT_FORMAT_PEM},
        {NULL,  0}
    };
    static Ns_ObjvTable keyImportSelections[] = {
        {"public",  NS_CRYPTO_KEYIMPORT_PUBLIC},
        {"private", NS_CRYPTO_KEYIMPORT_PRIVATE},
        {"keypair", NS_CRYPTO_KEYIMPORT_KEYPAIR},
        {NULL, 0}
    };
    Ns_ObjvSpec lopts[] = {
        {"!-name",    Ns_ObjvString, &typeName,    NULL},
        {"-from",     Ns_ObjvIndex,  &fromInt,     keyImportSelections},
        {"!-params",  Ns_ObjvObj,    &paramsObj,   NULL},
        {"-format",   Ns_ObjvIndex,  &formatInt,   keyImportFormats},
        {"-encoding", Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-outfile",  Ns_ObjvString, &outfileName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (Tcl_DictObjSize(interp, paramsObj, &dictSize) != TCL_OK) {
        Ns_TclPrintfResult(interp, "the option \"-params\" requires a dictionary");
        return TCL_ERROR;
    }
    (void)dictSize;

    if (formatInt != OUTPUT_FORMAT_RAW && encodingInt != -1) {
        Ns_TclPrintfResult(interp, "-encoding is only valid with -format raw");
        return TCL_ERROR;
    }

    selection = (Ns_CryptoKeyImportSelection)fromInt;
    encoding = (encodingInt == -1
                ? NS_OBJ_ENCODING_HEX
                : (Ns_BinaryEncoding)encodingInt);

    Ns_DListInit(&tmpData);
    Ns_DListSetFreeProc(&tmpData, FreelistFree);

    bld = OSSL_PARAM_BLD_new();
    if (bld == NULL) {
        SetResultFromOsslError(interp, "could not allocate key import parameter builder");
        goto done;
    }

    result = PkeyImportParamsFromDict(interp, typeName, selection, paramsObj,
                                      &resolvedTypeName, bld, &tmpData);
    if (result != TCL_OK) {
        goto done;
    }
    if (resolvedTypeName == NULL) {
        Ns_TclPrintfResult(interp, "internal error: unresolved key import type");
        result = TCL_ERROR;
        goto done;
    }

    params = OSSL_PARAM_BLD_to_param(bld);
    if (params == NULL) {
        SetResultFromOsslError(interp, "could not finalize key import parameters");
        result = TCL_ERROR;
        goto done;
    }
    result = PkeyImportFromParams(interp, resolvedTypeName, selection, params,
                                  formatInt, encoding, outfileName);

done:
    Ns_DListFree(&tmpData);

    if (params != NULL) {
        OSSL_PARAM_free(params);
    }
    if (bld != NULL) {
        OSSL_PARAM_BLD_free(bld);
    }
    return result;
}
#endif

/*----------------------------------------------------------------------
 *
 * CryptoKeyInfoObjCmd --
 *
 *      Implements "ns_crypto::key info". Returns a dictionary with
 *      information about a key provided in PEM format.
 *
 *      The result always contains at least the key type ("type").
 *      Additional fields are included depending on the key type and
 *      OpenSSL capabilities:
 *
 *        - "bits": key size in bits (RSA, DSA, DH, EC)
 *        - "curve": curve name for EC keys
 *
 *      The command is designed for lightweight inspection and may be
 *      extended in future versions to include additional properties.
 *
 * Results:
 *      TCL_OK on success, with a Tcl dictionary as result.
 *      TCL_ERROR on failure (e.g., invalid PEM).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKeyInfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    Ns_BinaryEncoding  encoding;
    const char        *pem = NULL, *passPhrase = NS_EMPTY_STRING;
    Tcl_Obj           *resultObj;
    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        EVP_PKEY *pkey = PkeyGetAnyFromPem(interp, pem, passPhrase);
        Tcl_Obj  *typeNameObj;

        encoding = (encodingInt == -1
                    ? NS_OBJ_ENCODING_HEX
                    : (Ns_BinaryEncoding)encodingInt);

        if (pkey == NULL) {
            return TCL_ERROR;
        }

        typeNameObj = PkeyTypeNameObj(interp, pkey);
        if (typeNameObj == NULL) {
            EVP_PKEY_free(pkey);
            return TCL_ERROR;
        }

        resultObj = Tcl_NewDictObj();

        Tcl_DictObjPut(interp, resultObj,
                       NsAtomObj(NS_ATOM_TYPE),
                       typeNameObj);

        /*
         * Basic/legacy-compatible details first.
         */
        if (PkeyInfoPutLegacyDetails(interp, resultObj, pkey) != TCL_OK) {
            EVP_PKEY_free(pkey);
            return TCL_ERROR;
        }

        if (PkeyInfoPutRsaDetails(interp, resultObj, pkey, encoding) != TCL_OK
            || PkeyInfoPutEcDetails(interp, resultObj, pkey, encoding) != TCL_OK
            || PkeyInfoPutOkpDetails(interp, resultObj, pkey, encoding) != TCL_OK
            ) {
            EVP_PKEY_free(pkey);
            return TCL_ERROR;
        }

# ifdef HAVE_OPENSSL_3
        /*
         * Optional provider-side enrichment. This must be best-effort:
         * if a key is not provider-backed, just skip these fields.
         */
        if (PkeyInfoPutProviderDetails(interp, resultObj, pkey) != TCL_OK) {
            EVP_PKEY_free(pkey);
            return TCL_ERROR;
        }
# endif

        if (PkeyInfoPutCapabilities(interp, resultObj, pkey) != TCL_OK) {
            EVP_PKEY_free(pkey);
            return TCL_ERROR;
        }

        Tcl_SetObjResult(interp, resultObj);
        result = TCL_OK;
        EVP_PKEY_free(pkey);
    }

    return result;
}

/*----------------------------------------------------------------------
 *
 * CryptoKeyPrivObjCmd --
 *
 *      Implements "ns_crypto::key priv". Extracts raw private key material
 *      from a PEM-encoded private key.
 *
 *      The input must contain a private key. When the key is encrypted,
 *      a passphrase may be provided.
 *
 *      Raw private key extraction is supported only for key types where
 *      OpenSSL exposes raw key material (e.g., EdDSA, ML-KEM). For other
 *      key types (e.g., EC, RSA), the operation is not supported and
 *      results in an error.
 *
 *      The output encoding is controlled via the "-encoding" option.
 *
 * Results:
 *      TCL_OK on success, with encoded private key as interpreter result.
 *      TCL_ERROR on failure or when extraction is not supported.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKeyPrivObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    const char        *pem = NULL;
    const char        *passPhrase = NS_EMPTY_STRING;
    EVP_PKEY          *pkey = NULL;
    Ns_BinaryEncoding  encoding;
    Ns_ObjvSpec        lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    encoding = (encodingInt == -1
                ? NS_OBJ_ENCODING_HEX
                : (Ns_BinaryEncoding)encodingInt);

    pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);
    if (pkey == NULL) {
        return TCL_ERROR;
    }

    result = SetResultFromRawPrivateKey(interp, pkey, encoding);

    EVP_PKEY_free(pkey);
    return result;
}

/*----------------------------------------------------------------------
 *
 * CryptoKeyPubObjCmd --
 *
 *      Implements "ns_crypto::key pub". Extracts raw public key material
 *      from a PEM-encoded key.
 *
 *      The input may be either a public or private key. When a private
 *      key is provided, the corresponding public key is derived.
 *
 *      The result is returned in an algorithm-specific raw format:
 *
 *        - EC: uncompressed point (0x04 || X || Y)
 *        - Provider-based keys (e.g., Ed25519, ML-KEM): exported raw key
 *          via OpenSSL APIs
 *
 *      The output encoding is controlled via the "-encoding" option.
 *
 * Results:
 *      TCL_OK on success, with encoded public key as interpreter result.
 *      TCL_ERROR on failure (e.g., invalid PEM, unsupported key type).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKeyPubObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                   TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1, formatInt = OUTPUT_FORMAT_RAW;
    const char        *pem = NULL;
    const char        *passPhrase = NS_EMPTY_STRING;
    const char        *outfileName = NULL;
    EVP_PKEY          *pkey = NULL;
    Ns_BinaryEncoding  encoding;
    static Ns_ObjvTable keyPubFormats[] = {
        {"raw", OUTPUT_FORMAT_RAW},
        {"pem", OUTPUT_FORMAT_PEM},
        {NULL,  0}
    };
    Ns_ObjvSpec        lopts[] = {
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-format",     Ns_ObjvIndex,  &formatInt,   keyPubFormats},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"-outfile",    Ns_ObjvString, &outfileName, NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    encoding = (encodingInt == -1
                ? NS_OBJ_ENCODING_HEX
                : (Ns_BinaryEncoding)encodingInt);

    pkey = PkeyGetAnyFromPem(interp, pem, passPhrase);
    if (pkey == NULL) {
        return TCL_ERROR;
    }

    if (formatInt == OUTPUT_FORMAT_RAW) {
        /*
         * Backward-compatible default: raw public key bytes in the
         * requested encoding.
         */
        if (outfileName != NULL) {
            Ns_TclPrintfResult(interp,
                               "the option \"-outfile\" requires \"-format pem\"");
            result = TCL_ERROR;
        } else {
            result = SetResultFromRawPublicKey(interp, pkey, encoding);
        }

    } else {
        /*
         * PEM-encoded SubjectPublicKeyInfo.
         */
        result = PkeyPublicPemWrite(interp,
                                   pkey,
                                   "key",
                                   "public",
                                   outfileName);
    }

    EVP_PKEY_free(pkey);
    return result;
}

/*----------------------------------------------------------------------
 *
 * CryptoKeyTypeObjCmd --
 *
 *      Implements "ns_crypto::key type". Determines the type of a key
 *      provided in PEM format and returns it as a lowercase string.
 *
 *      The command accepts either a PEM file name or PEM content. When
 *      the key is encrypted, a passphrase may be provided.
 *
 *      The returned type name is derived directly from OpenSSL. Under
 *      OpenSSL 3, EVP_PKEY_get0_type_name() is used; for older versions,
 *      the type is derived from EVP_PKEY_base_id().
 *
 * Results:
 *      TCL_OK on success, with the key type set as interpreter result.
 *      TCL_ERROR on failure (e.g., invalid PEM, unsupported key type).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoKeyTypeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result;
    const char *pem = NULL;
    const char *passPhrase = NS_EMPTY_STRING;
    EVP_PKEY   *pkey = NULL;
    Tcl_Obj    *typeNameObj;
    Ns_ObjvSpec lopts[] = {
        {"-passphrase", Ns_ObjvString, &passPhrase, NULL},
        {"!-pem",       Ns_ObjvString, &pem,        NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    pkey = PkeyGetAnyFromPem(interp, pem, passPhrase);
    if (pkey == NULL) {
        return TCL_ERROR;
    }
    typeNameObj = PkeyTypeNameObj(interp, pkey);
    if (typeNameObj == NULL) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, typeNameObj);
        result = TCL_OK;
    }

    EVP_PKEY_free(pkey);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoKeyObjCmd --
 *
 *      Implements "ns_crypto::key" with various subcommands to
 *      analyse (PEM) keys for its content.
 *
 * Results:
 *      Tcl Return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoKeyObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
# ifdef HAVE_OPENSSL_3
        {"generate", CryptoKeyGenerateObjCmd},
        {"import",   CryptoKeyImportObjCmd},
# endif
        {"info",     CryptoKeyInfoObjCmd},
        {"type",     CryptoKeyTypeObjCmd},
        {"pub",      CryptoKeyPubObjCmd},
        {"priv",     CryptoKeyPrivObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*======================================================================
 * Function Implementations: ns_crypto::signature
 *======================================================================
 */


/*
 *----------------------------------------------------------------------
 *
 * PkeySignatureDigestGet --
 *
 *      Determine the effective digest to be used for signature
 *      operations based on the requested digest and the key type.
 *
 *      For key types that require an external digest (e.g., RSA, EC),
 *      the provided digest is returned.
 *
 *      For key types with internal hashing (e.g., Ed25519, ML-DSA),
 *      the function enforces a NULL digest and may raise an error if
 *      a non-NULL digest was explicitly requested.
 *
 * Results:
 *      Standard Tcl result
 *
 * Side effects:
 *      May set the Tcl interpreter result in case of invalid digest
 *      usage for the given key type.
 *
 *----------------------------------------------------------------------
 */
static const char *
PkeySignatureDigestDefaultName(EVP_PKEY *pkey)
{
# ifdef HAVE_OPENSSL_3
    if (PkeyIsType(pkey, "SM2", EVP_PKEY_SM2) == 1) {
        return "sm3";
    }
# endif
    return  "sha256";
}

static int
PkeySignatureDigestGet(Tcl_Interp *interp, EVP_PKEY *pkey,
                      const char *digestName,
                      const EVP_MD **mdPtr)
{
    *mdPtr = NULL;

    if (!PkeySupportsSignature(pkey)) {
        Ns_TclPrintfResult(interp, "key type does not support signatures");
        return TCL_ERROR;
    }

    if (!PkeySignatureRequiresDigest(pkey)) {
        if (digestName != NULL) {
            Ns_TclPrintfResult(interp,
                               "digest specification is not supported for this key type");
            return TCL_ERROR;
        }
        return TCL_OK;
    }

    if (digestName == NULL) {
        digestName = PkeySignatureDigestDefaultName(pkey);
    }

    *mdPtr = EVP_get_digestbyname(digestName);
    if (*mdPtr == NULL) {
        Ns_TclPrintfResult(interp, "unknown digest \"%s\"", digestName);
        return TCL_ERROR;
    }

    return TCL_OK;
}

# ifdef HAVE_OPENSSL_3

static int
PkeySignatureInitSm2(Tcl_Interp *interp,
                     EVP_MD_CTX *mdctx,
                     EVP_PKEY *pkey,
                     const EVP_MD *md,
                     const unsigned char *id, size_t idLength,
                     bool sign,
                     EVP_PKEY_CTX **pctxPtr)
{
    static const unsigned char defaultSm2Id[] = "1234567812345678";
    EVP_PKEY_CTX              *pctx = NULL;
    const EVP_MD              *useMd = md;

    *pctxPtr = NULL;
    ERR_clear_error();

    if (id == NULL) {
        id = defaultSm2Id;
        idLength = sizeof(defaultSm2Id) - 1u;
    }

    if (idLength > (size_t)INT_MAX) {
        Ns_TclPrintfResult(interp, "SM2 identifier is too long");
        return TCL_ERROR;
    }

    pctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (pctx == NULL) {
        SetResultFromOsslError(interp, "could not allocate SM2 signature context");
        return TCL_ERROR;
    }

    if (EVP_PKEY_CTX_set1_id(pctx, id, (int)idLength) <= 0) {
        SetResultFromOsslError(interp, "could not set SM2 identifier");
        EVP_PKEY_CTX_free(pctx);
        return TCL_ERROR;
    }

    /*
     * SM2 is used through DigestSign/DigestVerify.  When no digest was
     * specified by the caller, default to SM3.
     */
    if (useMd == NULL) {
        useMd = EVP_sm3();
    }

    EVP_MD_CTX_set_pkey_ctx(mdctx, pctx);

    /*
     * Since mdctx already has an EVP_PKEY_CTX assigned, there is normally
     * no need to request another one from EVP_DigestSignInit().
     */
    if (sign) {
        if (EVP_DigestSignInit(mdctx, NULL, useMd, NULL, pkey) <= 0) {
            SetResultFromOsslError(interp, "could not initialize SM2 signature generation");
            EVP_PKEY_CTX_free(pctx);
            return TCL_ERROR;
        }
    } else {
        if (EVP_DigestVerifyInit(mdctx, NULL, useMd, NULL, pkey) <= 0) {
            SetResultFromOsslError(interp,
                                   "could not initialize SM2 signature verification");
            EVP_PKEY_CTX_free(pctx);
            return TCL_ERROR;
        }
    }
    *pctxPtr = pctx;
    return TCL_OK;
}
# endif

/*
 *----------------------------------------------------------------------
 *
 * PkeySignatureSign --
 *
 *      Compute a digital signature over the provided message using
 *      the specified private key.
 *
 *      The function automatically selects the correct signing mode
 *      depending on the key type:
 *
 *        - For classical algorithms (RSA, DSA, EC), an external digest
 *          is applied using EVP_DigestSign*().
 *
 *        - For modern algorithms (Ed25519, Ed448, ML-DSA), the message
 *          is passed directly to EVP_DigestSign*() with a NULL digest.
 *
 *      The resulting signature is returned in the Tcl interpreter
 *      result, encoded according to the requested output encoding.
 *
 * Results:
 *      TCL_OK on success, with the encoded signature as result.
 *      TCL_ERROR on failure, with an error message set.
 *
 * Side effects:
 *      Allocates temporary OpenSSL contexts and buffers.
 *      Sets the Tcl interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
PkeySignatureSign(Tcl_Interp *interp, EVP_PKEY *pkey,
                  const unsigned char *message, size_t messageLength,
                  const unsigned char *id, size_t idLength,
                  const EVP_MD *md, Ns_BinaryEncoding encoding)
{
    int            result = TCL_ERROR;
    EVP_MD_CTX    *mdctx = NULL;
    EVP_PKEY_CTX  *pctx = NULL;
    size_t         sigLen = 0u;
    unsigned char *sig = NULL;

    ERR_clear_error();

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL) {
        SetResultFromOsslError(interp, "could not allocate message digest context");
        goto done;
    }

#ifdef HAVE_OPENSSL_3
    if (PkeyIsType(pkey, "SM2", EVP_PKEY_SM2) == 1) {
        if (PkeySignatureInitSm2(interp, mdctx, pkey, md,
                                 id, idLength,
                                 NS_TRUE, &pctx) != TCL_OK) {
            goto done;
        }
    } else
#endif
    if (EVP_DigestSignInit(mdctx, &pctx, md, NULL, pkey) <= 0) {
        SetResultFromOsslError(interp, "could not initialize signature generation");
        goto done;
    }

    if (EVP_DigestSign(mdctx, NULL, &sigLen, message, messageLength) <= 0) {
        SetResultFromOsslError(interp, "could not determine signature length");
        goto done;
    }

    sig = ns_malloc(sigLen);
    if (sig == NULL) {
        SetResultFromOsslError(interp, "could not allocate signature buffer");
        goto done;
    }

    if (EVP_DigestSign(mdctx, sig, &sigLen, message, messageLength) <= 0) {
        SetResultFromOsslError(interp, "could not create signature");
        goto done;
    }

    Tcl_SetObjResult(interp, NsEncodedObj(sig, sigLen, NULL, encoding));
    result = TCL_OK;

done:
    if (sig != NULL) {
        ns_free(sig);
    }
    if (mdctx != NULL) {
        EVP_MD_CTX_free(mdctx);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PkeySignatureVerify --
 *
 *      Verify a digital signature over the provided message using
 *      the specified public or private key.
 *
 *      The function automatically selects the correct verification
 *      mode depending on the key type:
 *
 *        - For classical algorithms (RSA, DSA, EC), an external digest
 *          is applied using EVP_DigestVerify*().
 *
 *        - For modern algorithms (Ed25519, Ed448, ML-DSA), the message
 *          is passed directly to EVP_DigestVerify*() with a NULL digest.
 *
 * Results:
 *      TCL_OK with integer result:
 *          1  signature valid
 *          0  signature invalid
 *
 *      TCL_ERROR on failure (e.g., malformed signature or internal
 *      OpenSSL error), with an error message set.
 *
 * Side effects:
 *      Allocates temporary OpenSSL contexts.
 *      Sets the Tcl interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
PkeySignatureVerify(Tcl_Interp *interp, EVP_PKEY *pkey,
                    const unsigned char *message, size_t messageLength,
                    const unsigned char *signature, size_t signatureLength,
                    const unsigned char* id, size_t idLength,
                    const EVP_MD *md)
{
    int           rc, result = TCL_ERROR;
    EVP_MD_CTX   *mdctx = NULL;
    EVP_PKEY_CTX *pctx = NULL;

    ERR_clear_error();

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL) {
        SetResultFromOsslError(interp, "could not allocate message digest context");
        goto done;
    }

#ifdef HAVE_OPENSSL_3
    if (PkeyIsType(pkey, "SM2", EVP_PKEY_SM2) == 1) {
        if (PkeySignatureInitSm2(interp, mdctx, pkey, md,
                                 id, idLength, NS_FALSE, &pctx) != TCL_OK) {
            goto done;
        }
    } else
#endif
    if (EVP_DigestVerifyInit(mdctx, &pctx, md, NULL, pkey) <= 0) {
        SetResultFromOsslError(interp, "could not initialize signature verification");
        goto done;
    }

    rc = EVP_DigestVerify(mdctx, signature, signatureLength, message, messageLength);
    if (rc == 1) {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        result = TCL_OK;

    } else if (rc == 0) {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        result = TCL_OK;

    } else {
        SetResultFromOsslError(interp, "signature verification failed");
        result = TCL_ERROR;
    }

done:
    if (mdctx != NULL) {
        EVP_MD_CTX_free(mdctx);
    }
    return result;
}

static bool
PkeySignatureAcceptsId(EVP_PKEY *pkey)
{
#ifdef HAVE_OPENSSL_3
# ifdef EVP_PKEY_SM2
    return PkeyIsType(pkey, "SM2", EVP_PKEY_SM2) == 1;
# else
    return NS_FALSE;
# endif
#else
    return NS_FALSE;
#endif
}


static int
PkeySignatureAcceptsIdFromObj(Tcl_Interp *interp, EVP_PKEY *pkey, Tcl_Obj *idObj,
                              const unsigned char **idPtr, size_t *idLengthPtr)
{
    int result = TCL_OK;

    *idPtr = NULL;
    *idLengthPtr = 0u;

    if (PkeySignatureAcceptsId(pkey)) {
        if (idObj == NULL) {
            *idPtr = NULL;
            *idLengthPtr = 0;
        } else {
            TCL_SIZE_T idSize;

            *idPtr = (const unsigned char *)Tcl_GetStringFromObj(idObj, &idSize);
            *idLengthPtr  = (size_t)idSize;
        }
    } else {
        if (idObj != NULL) {
            Ns_TclPrintfResult(interp, "key type does not support parameter -id");
            result = TCL_ERROR;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoPkeySignatureSignObjCmd --
 *
 *      Implements the Tcl command:
 *
 *          ns_crypto::signature sign
 *
 *      Generate a digital signature over the provided message using
 *      a private key in PEM format.
 *
 *      The command supports both classical and modern signature
 *      algorithms. The appropriate signing mode (external digest or
 *      internal hashing) is selected automatically based on the key type.
 *
 * Results:
 *      TCL_OK on success, with the encoded signature as result.
 *      TCL_ERROR on failure, with an error message set.
 *
 * Side effects:
 *      Reads key material from PEM input.
 *      Allocates OpenSSL contexts.
 *      Sets the Tcl interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoPkeySignatureSignObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                          TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                  result, isBinary = 0, encodingInt = -1;
    const char          *pem = NULL, *passPhrase = NS_EMPTY_STRING, *digestName = NULL;
    const unsigned char *id = NULL;
    size_t               idLength = 0u;
    Tcl_Obj             *messageObj = NULL, *idObj = NULL;
    EVP_PKEY            *pkey = NULL;
    const EVP_MD        *md = NULL;
    const unsigned char *message;
    TCL_SIZE_T           messageLength;
    Tcl_DString          messageDs;
    Ns_BinaryEncoding    encoding;
    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString, &digestName,  NULL},
        {"-id",         Ns_ObjvObj,    &idObj,       NULL},
        {"-encoding",   Ns_ObjvIndex,  &encodingInt, NS_binaryencodings},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {"--",          Ns_ObjvBreak,  NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    encoding = (encodingInt == -1
                ? NS_OBJ_ENCODING_HEX
                : (Ns_BinaryEncoding)encodingInt);

    pkey = PkeyGetFromPem(interp, pem, passPhrase, NS_TRUE);
    if (pkey == NULL) {
        return TCL_ERROR;
    }

    if (!PkeySupportsSignature(pkey)) {
        Ns_TclPrintfResult(interp, "key type does not support signatures");
        result = TCL_ERROR;
        goto done;
    }

    if (PkeySignatureAcceptsIdFromObj(interp, pkey, idObj,
                                      &id, &idLength) != TCL_OK) {
        result = TCL_ERROR;
        goto done;
    }

    result = PkeySignatureDigestGet(interp, pkey, digestName, &md);
    if (result != TCL_OK) {
        goto done;
    }

    Tcl_DStringInit(&messageDs);
    message = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);

    result = PkeySignatureSign(interp, pkey, message, (size_t)messageLength,
                               id, idLength, md, encoding);

    Tcl_DStringFree(&messageDs);

done:
    EVP_PKEY_free(pkey);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoPkeySignatureVerifyObjCmd --
 *
 *      Implements the Tcl command:
 *
 *          ns_crypto::signature verify
 *
 *      Verify a digital signature over the provided message using
 *      a public or private key in PEM format.
 *
 *      The command supports both classical and modern signature
 *      algorithms. The appropriate verification mode is selected
 *      automatically based on the key type.
 *
 * Results:
 *      TCL_OK with integer result:
 *          1  signature valid
 *          0  signature invalid
 *
 *      TCL_ERROR on failure (e.g., malformed signature input or
 *      internal OpenSSL error).
 *
 * Side effects:
 *      Reads key material from PEM input.
 *      Allocates OpenSSL contexts.
 *      Sets the Tcl interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoPkeySignatureVerifyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                            TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                  result, isBinary = 0;
    const char          *pem = NULL, *passPhrase = NS_EMPTY_STRING, *digestName = NULL;
    const unsigned char *id = NULL;
    size_t               idLength;
    Tcl_Obj             *messageObj = NULL, *signatureObj = NULL, *idObj = NULL;
    EVP_PKEY            *pkey = NULL;
    const EVP_MD        *md = NULL;
    const unsigned char *message, *signature;
    TCL_SIZE_T           messageLength, signatureLength;
    Tcl_DString          messageDs;
    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,   &isBinary,    INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString, &digestName,  NULL},
        {"-id",         Ns_ObjvObj,    &idObj,       NULL},
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {"!-signature", Ns_ObjvObj,    &signatureObj, NULL},
        {"--",          Ns_ObjvBreak,  NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

   pkey = PkeyGetAnyFromPem(interp, pem, passPhrase);
    if (pkey == NULL) {
        return TCL_ERROR;
    }

    if (!PkeySupportsSignature(pkey)) {
        Ns_TclPrintfResult(interp, "key type does not support signatures");
        result = TCL_ERROR;
        goto done;
    }

    if (PkeySignatureAcceptsIdFromObj(interp, pkey, idObj,
                                      &id, &idLength) != TCL_OK) {
        result = TCL_ERROR;
        goto done;
    }

    result = PkeySignatureDigestGet(interp, pkey, digestName, &md);
    if (result != TCL_OK) {
        goto done;
    }

    Tcl_DStringInit(&messageDs);

    message = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);
    signature = (const unsigned char *)Tcl_GetByteArrayFromObj(signatureObj, &signatureLength);

    result = PkeySignatureVerify(interp, pkey,
                                 message, (size_t)messageLength,
                                 signature, (size_t)signatureLength,
                                 id, idLength,
                                 md);

    Tcl_DStringFree(&messageDs);

done:
    EVP_PKEY_free(pkey);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SignatureDefaultKeyName --
 *
 *      Determine a suitable default key type for signature key generation.
 *
 *      The function prefers modern algorithms when available:
 *      ML-DSA (post-quantum), Ed25519, EC, and finally RSA as fallback.
 *      Availability depends on the OpenSSL version, configured providers,
 *      and compile-time support.
 *
 * Results:
 *      Returns a pointer to a static string naming the key type.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
SignatureDefaultKeyName(void)
{
#ifdef HAVE_OPENSSL_3
    if (CryptoKeyTypeSupported("ML-DSA-65")) {
        return "ml-dsa-65";
    }
    if (CryptoKeyTypeSupported("ED25519")) {
        return "Ed25519";
    }
    if (CryptoKeyTypeSupported("EC")) {
        return "EC";
    }
    if (CryptoKeyTypeSupported("RSA")) {
        return "RSA";
    }
#else
# ifdef EVP_PKEY_ED25519
    return "Ed25519";
# elif defined(HAVE_OPENSSL_EC_H)
    return "EC";
# else
    return "RSA";
# endif
#endif
    return "RSA";
}

# ifdef HAVE_OPENSSL_3
/*
 *----------------------------------------------------------------------
 *
 * CryptoSignatureGenerateObjCmd --
 *
 *      Implements "ns_crypto::signature generate".
 *
 *      Generate a new signature key pair using OpenSSL provider-based
 *      key generation (e.g., ML-DSA).
 *
 *      The generated private key is written in PEM format either to
 *      a file or returned as command result.
 *
 * Results:
 *      TCL_OK on success, with PEM data or empty result when written
 *      to a file.
 *      TCL_ERROR on failure.
 *
 * Side effects:
 *      Allocates OpenSSL contexts and may write to a file.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoSignatureGenerateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                              TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const char *nameString = SignatureDefaultKeyName(), *groupName = NULL, *outfileName = NULL;
    OSSL_PARAM  params[2], *paramPtr = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-name",    Ns_ObjvString,  &nameString,  NULL},
        {"-group",   Ns_ObjvString, &groupName,   NULL},
        {"-outfile",  Ns_ObjvString, &outfileName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
      ns_crypto::signature generate -name ml-dsa-65 -outfile /tmp/ml-dsa-65.pem
      ns_crypto::signature generate -name ml-dsa-65
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else if (KeygenGroupParams(interp, nameString, groupName,
                                 "signature", params, &paramPtr) != TCL_OK) {
        return TCL_ERROR;   }

    return GeneratePrivateKeyPem(interp,
                                 nameString,
                                 "signature",
                                 outfileName,
                                 NS_CRYPTO_KEYGEN_USAGE_SIGNATURE,
                                 paramPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * CryptoSignaturePubObjCmd --
 *
 *      Implements "ns_crypto::signature pub".
 *
 *      Extract the public key corresponding to a signature key in
 *      PEM format.
 *
 *      The public key is returned or written as PEM encoded
 *      SubjectPublicKeyInfo ("BEGIN PUBLIC KEY").
 *
 * Results:
 *      TCL_OK on success.
 *      TCL_ERROR on failure.
 *
 * Side effects:
 *      Reads key material and may write to a file or set interpreter
 *      result.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoSignaturePubObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                         TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result;
    const char *pem = NULL, *passPhrase = NS_EMPTY_STRING, *outfileName = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-passphrase", Ns_ObjvString, &passPhrase,  NULL},
        {"-outfile",    Ns_ObjvString, &outfileName, NULL},
        {"!-pem",       Ns_ObjvString, &pem,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::signature pub -pem /tmp/mldsa.pem -outfile /tmp/pub.pem
      ns_crypto::signature pub -pem $pemString
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        EVP_PKEY *pkey = PkeyGetAnyFromPem(interp, pem, passPhrase);

        if (pkey == NULL) {
            result = TCL_ERROR;

        } else {
            result = PkeyPublicPemWrite(interp,
                                       pkey,
                                       "signature",
                                       "generated signature public key",
                                       outfileName);
            EVP_PKEY_free(pkey);
        }
    }
    return result;
}
# endif /* HAVE_OPENSSL_3 */

/*
 *----------------------------------------------------------------------
 *
 *  NsTclCryptoSignatureObjCmd --
 *
 *      Implements "ns_crypto::signature" with various subcommands to
 *      for signing and verification of signatures.
 *
 * Results:
 *      Tcl Return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoSignatureObjCmd(ClientData clientData, Tcl_Interp *interp,
                          TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
#ifdef HAVE_OPENSSL_3
        {"generate", CryptoSignatureGenerateObjCmd},
        {"pub",      CryptoSignaturePubObjCmd},
#endif
        {"sign",     CryptoPkeySignatureSignObjCmd},
        {"verify",   CryptoPkeySignatureVerifyObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*======================================================================
 * Function Implementations: ns_crypto::randombytes
 *======================================================================
 */


/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoRandomBytesObjCmd --
 *
 *        Implements "ns_crypto::randombytes". Returns random bytes
 *        from OpenSSL.
 *
 *        Example: ns_crypto::randombytes 20
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoRandomBytesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                result, nrBytes = 0, encodingInt = -1;
    Ns_ObjvValueRange  lengthRange = {1, INT_MAX};
    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex, &encodingInt, NS_binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"nrbytes",     Ns_ObjvInt,   &nrBytes,     &lengthRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? NS_OBJ_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        Tcl_DString ds;
        int         rc;

        Tcl_DStringInit(&ds);
        Tcl_DStringSetLength(&ds, (TCL_SIZE_T)nrBytes);
        rc = RAND_bytes((unsigned char *)ds.string, nrBytes);
        if (likely(rc == 1)) {
            Tcl_SetObjResult(interp, NsEncodedObj((unsigned char *)ds.string, (size_t)nrBytes, NULL, encoding));
            result = TCL_OK;
        } else {
            Ns_TclPrintfResult(interp, "could not obtain random bytes from OpenSSL");
            result = TCL_ERROR;
        }
        Tcl_DStringFree(&ds);
    }

    return result;
}


/*======================================================================
 * Function Implementations: ns_crypto::uuid
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * uuid_format --
 *
 *      Convert 16 raw bytes into the canonical RFC 4122 UUID string
 *      form (8-4-4-4-12 hex digits, lowercase, with hyphens). The
 *      caller must provide a destination buffer of at least 37 bytes
 *      to hold the 36-character UUID plus the trailing NUL.
 *
 * Results:
 *      None. The formatted UUID string is written into 'dst'.
 *
 *----------------------------------------------------------------------
 */

static inline char *
uuid_format(unsigned char *b, char *dst)
{
    /*
     * Format into canonical string 8-4-4-4-12
     */
    Ns_HexString(&b[0],  &dst[0],  4, NS_FALSE);  /* bytes 0..3   -> dst[0..7]   */
    dst[8]  = '-';

    Ns_HexString(&b[4],  &dst[9],  2, NS_FALSE);  /* bytes 4..5   -> dst[9..12]  */
    dst[13] = '-';

    Ns_HexString(&b[6],  &dst[14], 2, NS_FALSE);  /* bytes 6..7   -> dst[14..17] */
    dst[18] = '-';

    Ns_HexString(&b[8],  &dst[19], 2, NS_FALSE);  /* bytes 8..9   -> dst[19..22] */
    dst[23] = '-';

    Ns_HexString(&b[10], &dst[24], 6, NS_FALSE);  /* bytes 10..15 -> dst[24..35] */
    dst[36] = '\0';

    return dst;
}

/*
 *----------------------------------------------------------------------
 *
 * uuid_v4 --
 *
 *      Generate a Version 4 (random) RFC 4122 UUID string.  The
 *      function fills a 16-byte buffer with cryptographically strong
 *      random bytes via RAND_bytes() and Sets the UUID version and
 *      variant bits:
 *         * byte[6] high nibble -> 0b0100 (version 4)
 *         * byte[8] high bits   -> 0b10   (RFC 4122 variant)
 *
 *      Finally, it formats the result into canonical text using
 *      uuid_format().
 *
 * Results:
 *      Returns TCL_OK on success (UUID written into 'dst'),
 *      or TCL_ERROR if RAND_bytes() failed and no valid UUID was produced.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline const char *
uuid_v4(char *dst)
{
    unsigned char b[16];

    if (RAND_bytes(b, sizeof(b)) != 1) {
        return NULL;
    }

    /*
     * RFC 4122 version 4:
     * - Set the 4 most significant bits of byte 6 (index 6) to 0100 (0x4).
     * - Set the 2 most significant bits of byte 8 (index 8) to 10.
     *
     * Bytes are 0..15.
     */
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40); /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80); /* variant RFC4122 */

    return uuid_format(b, dst);
}


/*
 *----------------------------------------------------------------------
 *
 * uuid_v7 --
 *
 *      Summary: Generate a Version 7 (time-ordered) UUID per RFC 9562 and
 *      write its canonical textual form ("8-4-4-4-12") into 'dst'.
 *
 * Parameters:
 *      dst - Pointer to a character buffer that will receive the textual
 *            UUID; must not be NULL and must hold at least 37 bytes
 *            (36 printable characters plus terminating NUL).
 *
 * Returns:
 *      TCL_OK on success (a valid UUIDv7 string is written to 'dst' and NUL-terminated).
 *      TCL_ERROR on failure when RAND_bytes() reports an error; in this case the function
 *        does not guarantee a valid UUID in 'dst'.
 *
 * Side effects:
 *      None. The function calls Ns_GetTime(), Ns_TimeToMilliseconds(),
 *      RAND_bytes(), memcpy(), and uuid_format(); writes into 'dst'.
 *
 *----------------------------------------------------------------------
 */
static inline const char *
uuid_v7(char *dst)
{
    unsigned char   out[16];
    unsigned char   rnd[10];
    time_t          ms;
    unsigned long   b0, b1, b2;
    Ns_Time         now;

    /*
     * We need 10 random bytes:
     *  - parts of them go into the rand_a / rand_b fields.
     */
    if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
        return NULL;
    }

    /*
     * We'll compose the 16 output bytes:
     *
     * Bytes 0..5  : timestamp ms (big-endian 48-bit)
     * Byte  6     : high nibble = version(0x7), low nibble = top 4 bits of random
     * Byte  7     : next 8 random bits
     * Byte  8     : variant '10' in the top bits, then next 6 random bits
     * Byte  9..15 : remaining random bytes
     */

    Ns_GetTime(&now);
    ms = Ns_TimeToMilliseconds(&now);

    /* timestamp big-endian into out[0..5] */
    out[0] = (unsigned char)((ms >> 40) & 0xFF);
    out[1] = (unsigned char)((ms >> 32) & 0xFF);
    out[2] = (unsigned char)((ms >> 24) & 0xFF);
    out[3] = (unsigned char)((ms >> 16) & 0xFF);
    out[4] = (unsigned char)((ms >>  8) & 0xFF);
    out[5] = (unsigned char)( ms        & 0xFF);

    /*
     * Pull first 3 random bytes for the structured fields.
     */
    b0 = rnd[0];
    b1 = rnd[1];
    b2 = rnd[2];

    /*
     * Byte 6:
     *   high nibble = version (0111 -> 0x7)
     *   low  nibble = low 4 bits of b0
     */
    out[6] = (unsigned char)(0x70 | (b0 & 0x0F));

    /*
     * Byte 7:
     *   full 8 bits from b1
     */
    out[7] = (unsigned char)b1;

    /*
     * Byte 8:
     *   variant '10' in the two MSBs -> 0b10xxxxxx
     *   keep lower 6 bits from b2
     *
     *   mask lower 6 bits: b2 & 0x3F
     *   set top bits to 10: | 0x80
     */
    out[8] = (unsigned char)((b2 & 0x3F) | 0x80);

    /*
     * The rest (bytes 9..15) are just rnd[3]..rnd[9]
     */
    memcpy(&out[9], &rnd[3], 7);
    return uuid_format(out, dst);
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoUUIDCmd --
 *
 *        Implements "ns_crypto::uuid". Returns UUID in version v4
 *
 *        Example: ns_crypto::randombytes 20
 *
 * Results:
 *      Tcl Result Code.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoUUIDObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                 result, versionInt = 4;
    static Ns_ObjvTable uuidVersions[] = {
        {"v4",        4u},
        {"v7",        7u},
        {NULL,        0u}
    };
    Ns_ObjvSpec lopts[] = {
        {"-version",   Ns_ObjvIndex,   &versionInt, uuidVersions},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString ds;
        const char* hexString;

        Tcl_DStringInit(&ds);
        Tcl_DStringSetLength(&ds, (TCL_SIZE_T)36);

        if (versionInt == 4) {
            hexString = uuid_v4(ds.string);
        } else {
            hexString = uuid_v7(ds.string);
        }
        if (hexString != NULL) {
            Tcl_DStringResult(interp, &ds);
            result = TCL_OK;
        } else {
            Ns_TclPrintfResult(interp, "UUID conversion failed");
            result = TCL_ERROR;
        }
    }

    return result;
}

/*======================================================================
 * Function Implementations: "ns_info ssl -details" helpers
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * CryptoKeyTypeSupported --
 *
 *      Determine whether a specific cryptographic key type is supported
 *      by the underlying OpenSSL build.
 *
 *      The function checks for availability of key types such as RSA,
 *      EC, Ed25519/Ed448, X25519/X448, SM2, or post-quantum types like
 *      ML-KEM and ML-DSA. The result depends on the OpenSSL version,
 *      configured providers, and build-time options.
 *
 *      This function is used to populate the "keytypes" list returned by
 *      "ns_info ssl -details".
 *
 * Results:
 *      Returns NS_TRUE when the key type is available, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#ifdef HAVE_OPENSSL_3
static bool
CryptoKeyTypeSupported(const char *name)
{
    EVP_PKEY_CTX *ctx;
    bool          success = NS_FALSE;

    ctx = EVP_PKEY_CTX_new_from_name(NULL, name, NULL);
    if (ctx != NULL) {
        success = NS_TRUE;
        EVP_PKEY_CTX_free(ctx);
    }
    return success;
}
#else
static bool
CryptoKeyTypeSupported(const char *name)
{
    if (STRIEQ(name, "RSA")) {
        return NS_TRUE;
    }
# ifdef HAVE_OPENSSL_EC_H
    if (STRIEQ(name, "EC")) {
        return NS_TRUE;
    }
# endif
# ifdef HAVE_OPENSSL_SM2_H
    if (STRIEQ(name, "SM2")) {
        return NS_TRUE;
    }
# endif
    return NS_FALSE;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * CryptoSignatureSupported, CryptoAgreementSupported,
 * CryptoKemSupported, TrimmedLength --
 *
 *      Helper functions used to determine runtime capabilities of the
 *      OpenSSL-based cryptographic subsystem and to normalize metadata.
 *
 *      CryptoSignatureSupported:
 *          Checks whether the OpenSSL build supports asymmetric
 *          signature operations via EVP (e.g., RSA, ECDSA, EdDSA).
 *
 *      CryptoAgreementSupported:
 *          Checks whether key agreement mechanisms (e.g., ECDH, X25519)
 *          are available.
 *
 *      CryptoKemSupported:
 *          Checks whether key encapsulation mechanisms (e.g., ML-KEM)
 *          are supported by the active OpenSSL providers.
 *
 *      These functions are used when constructing the detailed result
 *      of "ns_info ssl -details", providing a structured view of
 *      available cryptographic capabilities and supported key types.
 *
 * Results:
 *      The functions return NS_TRUE or NS_FALSE depending on
 *      feature availability.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
CryptoSignatureSupported(void)
{
    return CryptoKeyTypeSupported("RSA")
        || CryptoKeyTypeSupported("EC")
        || CryptoKeyTypeSupported("ED25519")
        || CryptoKeyTypeSupported("ED448")
        || CryptoKeyTypeSupported("SM2")
        || CryptoKeyTypeSupported("ML-DSA-44");
}

static bool
CryptoAgreementSupported(void)
{
#ifdef HAVE_OPENSSL_3
    return CryptoKeyTypeSupported("X25519")
        || CryptoKeyTypeSupported("X448")
        || CryptoKeyTypeSupported("EC");
#else
    return NS_FALSE;
#endif
}

static bool
CryptoKemSupported(void)
{
    return CryptoKeyTypeSupported("ML-KEM-512");
}

/*
 *----------------------------------------------------------------------
 *
 * TrimmedLength --
 *
 *      Compute the length of a NUL-terminated string excluding trailing
 *      whitespace characters.
 *
 *      The function scans the string from the end and removes all
 *      characters for which isspace() returns true. The returned length
 *      can be used to create Tcl objects without copying or modifying
 *      the original string.
 *
 * Results:
 *      Returns the length of the string without trailing whitespace.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static TCL_SIZE_T
TrimmedLength(const char *s)
{
    TCL_SIZE_T len = (TCL_SIZE_T)strlen(s);

    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        len--;
    }
    return len;
}
/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoSSLDetailsObj --
 *
 *      Return a Tcl dictionary describing OpenSSL support in the
 *      current NaviServer build/runtime environment.
 *
 *      The dictionary contains a boolean field "enabled", compile-time
 *      and runtime OpenSSL version information, and a list of enabled
 *      "capabilities" and "keytypes".
 *
 * Results:
 *      Tcl dictionary object with SSL/OpenSSL details.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Tcl_Obj *
Ns_InfoSSLDetailsObj(void)
{
    Tcl_Obj *resultObj = Tcl_NewDictObj();
    Tcl_Obj *capabilitiesObj = Tcl_NewListObj(0, NULL);
    Tcl_Obj *keytypesObj = Tcl_NewListObj(0, NULL);
    unsigned long runtimeVersion = 0ul;
    const char   *runtimeVersionString = NULL;

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("enabled", TCL_INDEX_NONE),
                   Tcl_NewBooleanObj(1));

    /*
     * Compile-time version.
     */
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("headersVersion", TCL_INDEX_NONE),
                   Tcl_NewStringObj(OPENSSL_VERSION_TEXT, TrimmedLength(OPENSSL_VERSION_TEXT)));

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("headersVersionNumber", TCL_INDEX_NONE),
                   Tcl_NewWideIntObj((Tcl_WideInt)OPENSSL_VERSION_NUMBER));

    /*
     * Runtime version.
     */
# if OPENSSL_VERSION_NUMBER >= 0x10100000L
    runtimeVersion = OpenSSL_version_num();
    runtimeVersionString = OpenSSL_version(OPENSSL_VERSION);
# else
    runtimeVersion = SSLeay();
    runtimeVersionString = SSLeay_version(SSLEAY_VERSION);
# endif

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("runtimeVersion", TCL_INDEX_NONE),
                   Tcl_NewStringObj(runtimeVersionString, TrimmedLength(runtimeVersionString)));

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("runtimeVersionNumber", TCL_INDEX_NONE),
                   Tcl_NewWideIntObj((Tcl_WideInt)runtimeVersion));

    /*
     * Decoded major/minor/patch from runtime version.
     * This layout matches the traditional OpenSSL numeric format.
     */
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("major", TCL_INDEX_NONE),
                   Tcl_NewIntObj((int)((runtimeVersion >> 28) & 0xf)));

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("minor", TCL_INDEX_NONE),
                   Tcl_NewIntObj((int)((runtimeVersion >> 20) & 0xff)));

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("patch", TCL_INDEX_NONE),
                   Tcl_NewIntObj((int)((runtimeVersion >> 4) & 0xfff)));

    /*
     * Generic features.
     */
    Tcl_ListObjAppendElement(NULL, capabilitiesObj,
                             Tcl_NewStringObj("digest", TCL_INDEX_NONE));
    Tcl_ListObjAppendElement(NULL, capabilitiesObj,
                             Tcl_NewStringObj("cipher", TCL_INDEX_NONE));
    Tcl_ListObjAppendElement(NULL, capabilitiesObj,
                             Tcl_NewStringObj("hmac", TCL_INDEX_NONE));

    /*
     * Signature family.
     */
    if (CryptoSignatureSupported()) {
        Tcl_ListObjAppendElement(NULL, capabilitiesObj,
                                 Tcl_NewStringObj("signature", TCL_INDEX_NONE));
    }

    /*
     * Agreement family.
     */
    if (CryptoAgreementSupported()) {
        Tcl_ListObjAppendElement(NULL, capabilitiesObj,
                                 Tcl_NewStringObj("agreement", TCL_INDEX_NONE));
    }

    /*
     * KEM family.
     */
    if (CryptoKemSupported()) {
        Tcl_ListObjAppendElement(NULL, capabilitiesObj,
                                 Tcl_NewStringObj("kem", TCL_INDEX_NONE));
    }

    /*
     * Key-type / algorithm-family probes.
     *
     * These helper predicates use cheap runtime/provider probes where
     * available and conservative compile-time checks on legacy OpenSSL
     * versions. No real key generation is performed in the info path.
     */
    if (CryptoKeyTypeSupported("RSA")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("rsa", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("EC")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("ec", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("ED25519")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("ed25519", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("ED448")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("ed448", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("X25519")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("x25519", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("X448")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("x448", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("SM2")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("sm2", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("ML-KEM-512")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("ml-kem", TCL_INDEX_NONE));
    }
    if (CryptoKeyTypeSupported("ML-DSA-44")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("ml-dsa", TCL_INDEX_NONE));
    }
    /* 
     * Add synthetic key type as supported by ns_crypto::key import -name ...
     */
    if (CryptoKeyTypeSupported("ED25519")
        || CryptoKeyTypeSupported("ED448")
        || CryptoKeyTypeSupported("X25519")
        || CryptoKeyTypeSupported("X448")) {
        Tcl_ListObjAppendElement(NULL, keytypesObj,
                                 Tcl_NewStringObj("okp", TCL_INDEX_NONE));
    }
    

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("capabilities", TCL_INDEX_NONE),
                   capabilitiesObj);
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("keytypes", TCL_INDEX_NONE),
                   keytypesObj);

    return resultObj;
}


/*======================================================================
 * Backward compatibility checks
 *======================================================================
 */
# ifdef OPENSSL_NO_EC
int
NsTclCryptoEckeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "The used version of OpenSSL was built without EC support");
    return TCL_ERROR;
}
# endif

# ifndef HAVE_OPENSSL_3_5
int
NsTclCryptoKemObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "The used version of OpenSSL does not support ML-KEM");
    return TCL_ERROR;
}
# endif

# ifndef HAVE_OPENSSL_3
int
NsTclCryptoAgreementObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.0 or newer built into NaviServer");
    return TCL_ERROR;
}
# endif

#else
/*======================================================================
 * Compiled without OpenSSL support or too old OpenSSL versions
 *======================================================================
 */

int
NsTclCryptoHmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoMdObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoAeadDecryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}
int
NsTclCryptoAeadEncryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoRandomBytesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoUUIDCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoEckeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoScryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.0 or newer built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoPbkdf2hmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 1.1.1 or newer built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoArgon2ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.2 or newer built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoKeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoAgreementObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.0 or newer built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoSignatureObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

Tcl_Obj *
Ns_InfoSSLDetailsObj(void)
{
    Tcl_Obj *resultObj = Tcl_NewDictObj();
    Tcl_Obj *capabilitiesObj = Tcl_NewListObj(0, NULL);
    Tcl_Obj *keytypesObj = Tcl_NewListObj(0, NULL);

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("enabled", TCL_INDEX_NONE),
                   Tcl_NewBooleanObj(0));

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("capabilities", TCL_INDEX_NONE),
                   capabilitiesObj);
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("keytypes", TCL_INDEX_NONE),
                   keytypesObj);

    return resultObj;
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
