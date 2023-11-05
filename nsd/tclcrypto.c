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

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

# ifdef HAVE_OPENSSL_HKDF
#  include <openssl/kdf.h>
# endif

/*
 * The following result encodings can be used
 */
typedef enum {
    RESULT_ENCODING_HEX       = 1,
    RESULT_ENCODING_BASE64URL = 2,
    RESULT_ENCODING_BASE64    = 3,
    RESULT_ENCODING_BINARY    = 4
} Ns_BinaryEncoding;

static Ns_ObjvTable binaryencodings[] = {
    {"hex",      RESULT_ENCODING_HEX},
    {"base64url",RESULT_ENCODING_BASE64URL},
    {"base64",   RESULT_ENCODING_BASE64},
    {"binary",   RESULT_ENCODING_BINARY},
    {NULL,       0u}
};


/*
 * Static functions defined in this file.
 */
static Tcl_Obj *EncodedObj(
    unsigned char *octets,
    size_t octetLength,
    char *outputBuffer,
    Ns_BinaryEncoding encoding
) NS_GNUC_RETURNS_NONNULL NS_GNUC_NONNULL(1);

static int GetDigest(Tcl_Interp *interp, const char *digestName, const EVP_MD **mdPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

# ifndef OPENSSL_NO_EC
static int GetCurve(Tcl_Interp *interp, const char *curveName, int *nidPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void
SetResultFromEC_POINT(
    Tcl_Interp       *interp,
    Tcl_DString      *dsPtr,
    EC_KEY           *eckey,
    const EC_POINT   *ecpoint,
    BN_CTX           *bn_ctx,
    Ns_BinaryEncoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
# endif /* OPENSSL_NO_EC */

static int GetCipher(
  Tcl_Interp *interp, const char *cipherName, unsigned long flags,
  const char *modeMsg, const EVP_CIPHER **cipherPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

# ifndef HAVE_OPENSSL_PRE_1_0
static void ListMDfunc(const EVP_MD *m, const char *from, const char *to, void *arg);
# endif

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

# ifndef OPENSSL_NO_EC
#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
static TCL_OBJCMDPROC_T CryptoEckeyPrivObjCmd;
static TCL_OBJCMDPROC_T CryptoEckeyImportObjCmd;
#  endif
# endif

/*
 * Local variables defined in this file.
 */

static const char * const mdCtxType  = "ns:mdctx";
static const char * const hmacCtxType  = "ns:hmacctx";

# ifdef HAVE_OPENSSL_HKDF
static Ns_ObjvValueRange posIntRange0 = {0, INT_MAX};
# endif
# ifdef HAVE_OPENSSL_3
#include <openssl/core_names.h>
# endif
static Ns_ObjvValueRange posIntRange1 = {1, INT_MAX};

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
 * EncodedObj --
 *
 *      Helper function result encodings.
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      Interp result Obj is updated in case of error.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj*
EncodedObj(unsigned char *octets, size_t octetLength,
           char *outputBuffer, Ns_BinaryEncoding encoding) {
    char    *origOutputBuffer = outputBuffer;
    Tcl_Obj *resultObj = NULL; /* enumeration is complete, quiet some older compilers */

    NS_NONNULL_ASSERT(octets != NULL);

    if (outputBuffer == NULL && encoding != RESULT_ENCODING_BINARY) {
        /*
         * It is a safe assumption to double the size, since the hex
         * encoding needs the most space.
         */
        outputBuffer = ns_malloc(octetLength * 2u + 1u);
    }

    switch (encoding) {
    case RESULT_ENCODING_BINARY:
        resultObj = Tcl_NewByteArrayObj(octets, (TCL_SIZE_T)octetLength);
        break;

    case RESULT_ENCODING_BASE64URL:
        hexPrint("result", octets, octetLength);
        (void)Ns_HtuuEncode2(octets, octetLength, outputBuffer, 1);
        resultObj = Tcl_NewStringObj(outputBuffer, (TCL_SIZE_T)strlen(outputBuffer));
        break;

    case RESULT_ENCODING_BASE64:
        (void)Ns_HtuuEncode2(octets, octetLength, outputBuffer, 0);
        resultObj = Tcl_NewStringObj(outputBuffer, (TCL_SIZE_T)strlen(outputBuffer));
        break;

    case RESULT_ENCODING_HEX:
        Ns_HexString(octets, outputBuffer, (TCL_SIZE_T)octetLength, NS_FALSE);
        resultObj = Tcl_NewStringObj(outputBuffer, (TCL_SIZE_T)octetLength*2);
        break;
    }

    if (outputBuffer != origOutputBuffer) {
        ns_free(outputBuffer);
    }

    return resultObj;
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
# endif


/*
 *----------------------------------------------------------------------
 *
 * GetDigest, ListMDfunc --
 *
 *      Converter from a digest string to internal OpenSSL
 *      representation.  ListMDfunc is an iterator usable in OpenSSL
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

# ifndef HAVE_OPENSSL_PRE_1_0
static void
ListMDfunc(const EVP_MD *m, const char *from, const char *UNUSED(to), void *arg)
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

static int
GetDigest(Tcl_Interp *interp, const char *digestName, const EVP_MD **mdPtr)
{
    int result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(digestName != NULL);
    NS_NONNULL_ASSERT(mdPtr != NULL);

    *mdPtr = EVP_get_digestbyname(digestName);
    if (*mdPtr == NULL) {
# ifndef HAVE_OPENSSL_PRE_1_0
        /*
         * EVP_MD_do_all_sorted was added in OpenSSL 1.0.0. The
         * function is an iterator, which we provide with a tailored
         * callback.
         */
        Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

        Tcl_IncrRefCount(listObj);
        EVP_MD_do_all_sorted(ListMDfunc, listObj);
        Ns_TclPrintfResult(interp, "Unknown value for digest \"%s\", valid: %s",
                           digestName, Tcl_GetString(listObj));
        Tcl_DecrRefCount(listObj);
# else
        Ns_TclPrintfResult(interp, "Unknown message digest \"%s\"", digestName);
# endif
        result = TCL_ERROR;
    } else {
        result = TCL_OK;
    }
    return result;
}

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
 * GetCurve --
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
GetCurve(Tcl_Interp *interp, const char *curveName, int *nidPtr)
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
    if (strcmp(curveName, "secp192r1") == 0) {
        Ns_Log(Warning, "using curve name prime192v1 instead of secp192r1");
        nid = NID_X9_62_prime192v1;
    } else if (strcmp(curveName, "secp256r1") == 0) {
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
 * GetPkeyFromPem, GetEckeyFromPem --
 *
 *      Helper function for reading .pem-files
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      Interp result Obj is updated in case of error.
 *
 *----------------------------------------------------------------------
 */

static EVP_PKEY *
GetPkeyFromPem(Tcl_Interp *interp, char *pemFileName, const char *passPhrase, bool private)
{
    BIO        *bio;
    EVP_PKEY   *result;

    bio = BIO_new_file(pemFileName, "r");
    if (bio == NULL) {
        Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", pemFileName);
        result = NULL;
    } else {
        if (private) {
            result = PEM_read_bio_PrivateKey(bio, NULL, NULL, (char*)passPhrase);
        } else {
            result = PEM_read_bio_PUBKEY(bio, NULL, NULL, (char*)passPhrase);
        }
        BIO_free(bio);
        if (result == NULL) {
            Ns_TclPrintfResult(interp, "pem file contains no %s key", (private ? "private" : "public"));
        }
    }
    return result;
}

# ifndef OPENSSL_NO_EC
static EC_KEY *
GetEckeyFromPem(Tcl_Interp *interp, char *pemFileName, const char *passPhrase, bool private)
{
    BIO        *bio;
    EC_KEY     *result;

    bio = BIO_new_file(pemFileName, "r");
    if (bio == NULL) {
        Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", pemFileName);
        result = NULL;
    } else {
        if (private) {
            result = PEM_read_bio_ECPrivateKey(bio, NULL, NULL, (char*)passPhrase);
        } else {
            result = PEM_read_bio_EC_PUBKEY(bio, NULL, NULL, (char*)passPhrase);
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
CryptoHmacNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int         result, isBinary = 0;
    char       *digestName = (char *)"sha256";
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
        const EVP_MD  *md;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {
            HMAC_CTX            *ctx;
            const unsigned char *keyString;
            TCL_SIZE_T           keyLength;
            Tcl_DString          keyDs;

            Tcl_DStringInit(&keyDs);
            keyString = Ns_GetBinaryString(keyObj, isBinary == 1, &keyLength, &keyDs);
            ctx = HMAC_CTX_new();
            HMAC_Init_ex(ctx, keyString, (int)keyLength, md, NULL);
            Ns_TclSetAddrObj(Tcl_GetObjResult(interp), hmacCtxType, ctx);
            Tcl_DStringFree(&keyDs);
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
CryptoHmacAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
CryptoHmacGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK, encodingInt = -1;
    HMAC_CTX          *ctx;
    Tcl_Obj           *ctxObj;

    Ns_ObjvSpec    lopts[] = {
        {"-encoding", Ns_ObjvIndex,  &encodingInt,  binaryencodings},
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
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
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
        Tcl_SetObjResult(interp, EncodedObj(digest, mdLength, digestChars, encoding));
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
CryptoHmacFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
CryptoHmacStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *keyObj, *messageObj;
    char              *digestName = (char *)"sha256";

    Ns_ObjvSpec    lopts[] = {
        {"-binary",   Ns_ObjvBool,     &isBinary,    INT2PTR(NS_TRUE)},
        {"-digest",   Ns_ObjvString,   &digestName,  NULL},
        {"-encoding", Ns_ObjvIndex,    &encodingInt, binaryencodings},
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
        const EVP_MD  *md;
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);

        /*
         * Look up the Message digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {
            unsigned char        digest[EVP_MAX_MD_SIZE];
            char                 digestChars[EVP_MAX_MD_SIZE*2 + 1];
            HMAC_CTX            *ctx;
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
            ctx = HMAC_CTX_new();
            HMAC(md,
                 (const void *)keyString, (int)keyLength,
                 (const void *)messageString, (size_t)messageLength,
                 digest, &mdLength);
            HMAC_CTX_free(ctx);

            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            Tcl_SetObjResult(interp, EncodedObj(digest, mdLength, digestChars, encoding));

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
 *      NS_OK
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCryptoHmacObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
CryptoMdNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int           result;
    char         *digestName = (char *)"sha256";
    Ns_ObjvSpec   args[] = {
        {"digest",  Ns_ObjvString, &digestName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const EVP_MD  *md;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {
            EVP_MD_CTX    *mdctx;

            mdctx = NS_EVP_MD_CTX_new();
            EVP_DigestInit_ex(mdctx, md, NULL);
            Ns_TclSetAddrObj(Tcl_GetObjResult(interp), mdCtxType, mdctx);
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
CryptoMdAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
CryptoMdGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK, encodingInt = -1;
    EVP_MD_CTX        *mdctx;
    Tcl_Obj           *ctxObj;

    Ns_ObjvSpec lopts[] = {
        {"-encoding", Ns_ObjvIndex, &encodingInt, binaryencodings},
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
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
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
        Tcl_SetObjResult(interp, EncodedObj(digest, mdLength, digestChars, encoding));
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
CryptoMdFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
CryptoMdStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *messageObj, *signatureObj = NULL, *resultObj = NULL;
    char              *digestName = (char *)"sha256",
                      *passPhrase = (char *)NS_EMPTY_STRING,
                      *signKeyFile = NULL,
                      *verifyKeyFile = NULL;

    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,     &isBinary,         INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString,   &digestName,       NULL},
        {"-encoding",   Ns_ObjvIndex,    &encodingInt,      binaryencodings},
        {"-passphrase", Ns_ObjvString,   &passPhrase,       NULL},
        {"-sign",       Ns_ObjvString,   &signKeyFile,      NULL},
        {"-signature",  Ns_ObjvObj,      &signatureObj,     NULL},
        {"-verify",     Ns_ObjvString,   &verifyKeyFile,    NULL},
        {"--",          Ns_ObjvBreak,    NULL,              NULL},
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
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        const EVP_MD *md;
        EVP_PKEY     *pkey = NULL;
        char         *keyFile = NULL;

        /*
         * Compute Message Digest or sign or validate signature via OpenSSL.
         *
         * ::ns_crypto::md string -digest sha256 -sign /usr/local/src/naviserver/private.pem "hello\n"
         *
         * Example from https://medium.com/@bn121rajesh/rsa-sign-and-verify-using-openssl-behind-the-scene-bf3cac0aade2
         * ::ns_crypto::md string -digest sha1 -sign /usr/local/src/naviserver/myprivate.pem "abcdefghijklmnopqrstuvwxyz\n"
         *
         */
        result = GetDigest(interp, digestName, &md);
        if (signKeyFile != NULL) {
            keyFile = signKeyFile;
        } else if (verifyKeyFile != NULL) {
            keyFile = verifyKeyFile;
        }
        if (result != TCL_ERROR && keyFile != NULL) {
            pkey = GetPkeyFromPem(interp, keyFile, passPhrase, (signKeyFile != NULL));
            if (pkey == NULL) {
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR) {
            unsigned char        digestBuffer[EVP_MAX_MD_SIZE], *digest = digestBuffer;
            char                 digestChars[EVP_MAX_MD_SIZE*2 + 1], *outputBuffer = digestChars;
            EVP_MD_CTX          *mdctx;
            const unsigned char *messageString;
            TCL_SIZE_T           messageLength;
            unsigned int         mdLength = 0u;
            Tcl_DString          messageDs, signatureDs;

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
            mdctx = NS_EVP_MD_CTX_new();
            if (signKeyFile != NULL || verifyKeyFile != NULL) {
                EVP_PKEY_CTX  *pctx;
                int            r;

                if (signKeyFile != NULL) {
                    r =  EVP_DigestSignInit(mdctx, &pctx, md, NULL /*engine*/, pkey);
                } else {
                    r =  EVP_DigestVerifyInit(mdctx, &pctx, md, NULL /*engine*/, pkey);
                }

                if (r == 0) {
                    Ns_TclPrintfResult(interp, "could not initialize signature context");
                    result = TCL_ERROR;
                    pctx = NULL;
                } else {
                    size_t mdSize;

                    if (signKeyFile != NULL) {
                        /*
                         * A sign operation was requested.
                         */
                        r = EVP_DigestSignUpdate(mdctx, messageString, (size_t)messageLength);

                        if (r == 1) {
                            r = EVP_DigestSignFinal(mdctx, NULL, &mdSize);
                            if (r == 1) {
                                /*
                                 * Everything was fine, get a buffer
                                 * with the requested size and use
                                 * this as "digest".
                                 */
                                Tcl_DStringSetLength(&signatureDs, (TCL_SIZE_T)mdSize);
                                digest = (unsigned char*)signatureDs.string;

                                r = EVP_DigestSignFinal(mdctx, digest, &mdSize);

                                outputBuffer = ns_malloc(mdSize * 2u + 1u);
                                mdLength = (unsigned int)mdSize;
                                mdctx = NULL;
                            } else {
                                char errorBuffer[256];

                                Ns_TclPrintfResult(interp, "error while signing input: %s",
                                                   ERR_error_string(ERR_get_error(), errorBuffer));
                                result = TCL_ERROR;
                                mdctx = NULL;
                            }
                        }
                        if (r != 1) {
                            Ns_TclPrintfResult(interp, "error while signing input");
                            result = TCL_ERROR;
                        }
                    } else {
                        /*
                         * A signature verification was requested.
                         */
                        r = EVP_DigestVerifyUpdate(mdctx,
                                                   messageString,
                                                   (size_t)messageLength);

                        if (r == 1) {
                            TCL_SIZE_T           signatureLength;
                            const unsigned char *signatureString;

                            signatureString = Ns_GetBinaryString(signatureObj, 1,
                                                                 &signatureLength,
                                                                 &signatureDs);
                            r = EVP_DigestVerifyFinal(mdctx,
                                                      signatureString,
                                                      (size_t)signatureLength);

                            if (r == 1) {
                                /*
                                 * The signature was successfully verified.
                                 */
                                resultObj = Tcl_NewIntObj(1);
                                mdctx = NULL;
                            } else if (r == 0) {
                                /*
                                 * Signature verification failure.
                                 */
                                resultObj = Tcl_NewIntObj(0);
                                mdctx = NULL;
                            } else {
                                Ns_TclPrintfResult(interp, "error while verifying signature");
                                result = TCL_ERROR;
                            }
                        } else {
                            Ns_TclPrintfResult(interp, "error while updating verify digest");
                            result = TCL_ERROR;
                        }
                    }
                }
                if (pctx != NULL) {
                    EVP_PKEY_CTX_free(pctx);
                }
                EVP_PKEY_free(pkey);

            } else {
                EVP_DigestInit_ex(mdctx, md, NULL);
                EVP_DigestUpdate(mdctx, messageString, (unsigned long)messageLength);
                EVP_DigestFinal_ex(mdctx, digest, &mdLength);
            }

            if (mdctx != NULL) {
                NS_EVP_MD_CTX_free(mdctx);
            }

            if (result == TCL_OK) {
                /*
                 * Convert the result to the requested output format,
                 * unless we have already some resultObj.
                 */
                if (resultObj == NULL) {
                    resultObj = EncodedObj(digest, mdLength, outputBuffer, encoding);
                }

                Tcl_SetObjResult(interp, resultObj);
            }
            if (outputBuffer != digestChars) {
                ns_free(outputBuffer);
            }
            Tcl_DStringFree(&messageDs);
            Tcl_DStringFree(&signatureDs);
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
CryptoMdVapidSignObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *messageObj;
    char              *digestName = (char *)"sha256", *pemFile = NULL,
                      *passPhrase = (char *)NS_EMPTY_STRING;
    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,        &isBinary,   INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString,      &digestName, NULL},
        {"-encoding",   Ns_ObjvIndex,       &encodingInt,binaryencodings},
        {"-passphrase", Ns_ObjvString,      &passPhrase, NULL},
        {"-pem",        Ns_ObjvString,      &pemFile,    NULL},
        {"--",          Ns_ObjvBreak,       NULL,        NULL},
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

    } else if (pemFile == NULL) {
        Ns_TclPrintfResult(interp, "no pem file specified");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        const EVP_MD *md;
        EC_KEY       *eckey = NULL;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {

            eckey = GetEckeyFromPem(interp, pemFile, passPhrase, NS_TRUE);
            if (eckey == NULL) {
                /*
                 * GetEckeyFromPem handles error message
                 */
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR) {
            unsigned char        digest[EVP_MAX_MD_SIZE];
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

            EVP_DigestInit_ex(mdctx, md, NULL);
            EVP_DigestUpdate(mdctx, messageString, (unsigned long)messageLength);
            EVP_DigestFinal_ex(mdctx, digest, &mdLength);

            sig = ECDSA_do_sign(digest, SHA256_DIGEST_LENGTH, eckey);
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
            Tcl_SetObjResult(interp, EncodedObj(rawSig, sigLen, NULL, encoding));

            /*
             * Clean up.
             */
            EC_KEY_free(eckey);
            NS_EVP_MD_CTX_free(mdctx);
            ns_free(rawSig);
            Tcl_DStringFree(&messageDs);
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
CryptoMdHkdfObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, outLength = 0, encodingInt = -1;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL, *infoObj = NULL;
    char              *digestName = (char *)"sha256";
    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,           &isBinary,  INT2PTR(NS_TRUE)},
        {"-digest",   Ns_ObjvString,         &digestName, NULL},
        {"-salt",     Ns_ObjvObj,            &saltObj,    NULL},
        {"-secret",   Ns_ObjvObj,            &secretObj,  NULL},
        {"-info",     Ns_ObjvObj,            &infoObj,    NULL},
        {"-encoding", Ns_ObjvIndex,          &encodingInt,binaryencodings},
        {"--",        Ns_ObjvBreak,          NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"length", Ns_ObjvInt, &outLength, &posIntRange0},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ::ns_crypto::md hkdf -digest sha256 -salt foo -secret var -info "Content-Encoding: auth" 10

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

    } else if (saltObj == NULL) {
        Ns_TclPrintfResult(interp, "no -salt specified");
        result = TCL_ERROR;

    } else if (secretObj == NULL) {
        Ns_TclPrintfResult(interp, "no -secret specified");
        result = TCL_ERROR;

    } else if (infoObj == NULL) {
        Ns_TclPrintfResult(interp, "no -info specified");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        const EVP_MD *md;
        EVP_PKEY_CTX *pctx = NULL;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);

        if (result != TCL_ERROR) {
            pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
            if (pctx == NULL) {
                Ns_TclPrintfResult(interp, "could not obtain context HKDF");
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR && (EVP_PKEY_derive_init(pctx) <= 0)) {
            Ns_TclPrintfResult(interp, "could not initialize for derivation");
            result = TCL_ERROR;
        }
        if (result != TCL_ERROR && (EVP_PKEY_CTX_set_hkdf_md(pctx, md) <= 0)) {
            Ns_TclPrintfResult(interp, "could not set digest algorithm");
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
                Tcl_SetObjResult(interp, EncodedObj(keyString, outSize, NULL, encoding));
            }

            /*
             * Clean up.
             */
            ns_free((char*)keyString);
            Tcl_DStringFree(&saltDs);
            Tcl_DStringFree(&secretDs);
            Tcl_DStringFree(&infoDs);
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
 *      NS_OK
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoMdObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",    CryptoMdStringObjCmd},
        {"new",       CryptoMdNewObjCmd},
        {"add",       CryptoMdAddObjCmd},
        {"get",       CryptoMdGetObjCmd},
        {"free",      CryptoMdFreeObjCmd},
# ifndef OPENSSL_NO_EC
        {"vapidsign", CryptoMdVapidSignObjCmd},
# endif
# ifdef HAVE_OPENSSL_HKDF
        {"hkdf",      CryptoMdHkdfObjCmd},
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
NsTclCryptoScryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, nValue = 1024, rValue = 8, pValue = 16, encodingInt = -1;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,    &isBinary,  INT2PTR(NS_TRUE)},
        {"-salt",     Ns_ObjvObj,     &saltObj,    NULL},
        {"-secret",   Ns_ObjvObj,     &secretObj,  NULL},
        {"-n",        Ns_ObjvInt,     &nValue,     &posIntRange1},
        {"-p",        Ns_ObjvInt,     &pValue,     &posIntRange1},
        {"-r",        Ns_ObjvInt,     &rValue,     &posIntRange1},
        {"-encoding", Ns_ObjvIndex,   &encodingInt,binaryencodings},
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

    } else if (saltObj == NULL) {
        Ns_TclPrintfResult(interp, "no -salt specified");
        result = TCL_ERROR;

    } else if (secretObj == NULL) {
        Ns_TclPrintfResult(interp, "no -secret specified");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding    encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
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
                                                 (void*)secretString, (size_t)secretLength);
        *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                 (void*)saltString, (size_t)saltLength);
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

            Tcl_SetObjResult(interp, EncodedObj(out, sizeof(out), NULL, encoding));
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
NsTclCryptoScryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
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
NsTclCryptoArgon2ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1,
                       memcost = 1024, iter = 3, lanes = 1, threads = 1, outlen = 64;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL, *adObj = NULL, *passObj = NULL;
    const char        *variant = "Argon2id";
    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,    &isBinary,  INT2PTR(NS_TRUE)},
        {"-ad",       Ns_ObjvObj,     &adObj,      NULL},
        {"-iter",     Ns_ObjvInt,     &iter,       &posIntRange1},
        {"-lanes",    Ns_ObjvInt,     &lanes,      &posIntRange1},
        {"-memcost",  Ns_ObjvInt,     &memcost,    &posIntRange1},
        {"-outlen",   Ns_ObjvInt,     &outlen,     &posIntRange1},
        {"-password", Ns_ObjvObj,     &passObj,    NULL},
        {"-salt",     Ns_ObjvObj,     &saltObj,    NULL},
        {"-secret",   Ns_ObjvObj,     &secretObj,  NULL},
        {"-threads",  Ns_ObjvInt,     &threads,    NULL},
        {"-variant",  Ns_ObjvString,  &variant,    NULL},
        {"-encoding", Ns_ObjvIndex,   &encodingInt,binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (saltObj == NULL) {
        Ns_TclPrintfResult(interp, "no -salt specified");
        result = TCL_ERROR;

    } else if (threads > lanes)  {
        Ns_TclPrintfResult(interp, "requested more threads than lanes");
        result = TCL_ERROR;

    } else if (memcost < 8 * lanes) {
        Ns_TclPrintfResult(interp, "memcost must be greater or equal than 8 times the number of lanes");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding    encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
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
                                                 (void*)saltString, (size_t)saltLength);

        if (secretObj != NULL) {
            secretString = Ns_GetBinaryString(secretObj, isBinary == 1, &secretLength, &secretDs);
            //NsHexPrint("secretString", secretString, (size_t)secretLength, 32, NS_FALSE);
            *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET,
                                                     (void*)secretString, (size_t)secretLength);
        }
        if (adObj != NULL) {
            adString = Ns_GetBinaryString(adObj,     isBinary == 1, &adLength,     &adDs);
            //NsHexPrint("adString", adString, (size_t)adLength, 32, NS_FALSE);
            *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_ARGON2_AD,
                                                     (void*)adString, (size_t)adLength);
        }
        if (passObj != NULL) {
            passString = Ns_GetBinaryString(passObj, isBinary == 1, &passLength,   &passDs);
            //NsHexPrint("passString", passString, (size_t)passLength, 32, NS_FALSE);
            *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                                     (void*)passString, (size_t)passLength);
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

        if (EVP_KDF_CTX_set_params(kctx, params) <= 0) {
            Ns_TclPrintfResult(interp, "argon2: could not set parameters");
            result = TCL_ERROR;

        } else if (EVP_KDF_derive(kctx, (unsigned char *)outDs.string, (size_t)outlen, params) <= 0) {
            Ns_TclPrintfResult(interp, "argon2: could not derive key");
            result = TCL_ERROR;
        }  else {
            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            //fprintf(stderr, "Output = %s\n", OPENSSL_buf2hexstr((unsigned char *)outDs.string, outlen));

            Tcl_SetObjResult(interp, EncodedObj((unsigned char *)outDs.string, (size_t)outlen, NULL, encoding));
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
NsTclCryptoArgon2ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
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
NsTclCryptoPbkdf2hmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1, iter = 4096, dkLength = -1;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL;
    char              *digestName = (char *)"sha256";
    Ns_ObjvSpec opts[] = {
        {"-binary",     Ns_ObjvBool,    &isBinary,   INT2PTR(NS_TRUE)},
        {"-digest",     Ns_ObjvString,  &digestName, NULL},
        {"-dklen",      Ns_ObjvInt,     &dkLength,   &posIntRange1},
        {"-iterations", Ns_ObjvInt,     &iter,       &posIntRange1},
        {"-salt",       Ns_ObjvObj,     &saltObj,    NULL},
        {"-secret",     Ns_ObjvObj,     &secretObj,  NULL},
        {"-encoding",   Ns_ObjvIndex,   &encodingInt,binaryencodings},
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

    } else if (saltObj == NULL) {
        Ns_TclPrintfResult(interp, "no -salt specified");
        result = TCL_ERROR;

    } else if (secretObj == NULL) {
        Ns_TclPrintfResult(interp, "no -secret specified");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        const EVP_MD     *md;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
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
                dkLength = EVP_MD_size(md);
            }
            out = ns_malloc((size_t)dkLength);

            saltString   = Ns_GetBinaryString(saltObj,   isBinary == 1, &saltLength,   &saltDs);
            secretString = Ns_GetBinaryString(secretObj, isBinary == 1, &secretLength, &secretDs);

            if (PKCS5_PBKDF2_HMAC((const char *)secretString, (int)secretLength,
                                  saltString, (int)saltLength,
                                  iter, md,
                                  dkLength, out) == 1) {
                Tcl_SetObjResult(interp, EncodedObj(out, (size_t)dkLength, NULL, encoding));
                result = TCL_OK;
            } else {
                Ns_TclPrintfResult(interp, "could not derive key");
                result = TCL_ERROR;
            }
            ns_free(out);
        }
    }
    return result;
}



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
CryptoEckeyPrivObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    char              *pemFile = NULL,
                      *passPhrase = (char *)NS_EMPTY_STRING;

    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,   &encodingInt,binaryencodings},
        {"-passphrase", Ns_ObjvString,  &passPhrase, NULL},
        {"-pem",        Ns_ObjvString,  &pemFile,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::eckey priv -pem /usr/local/ns/modules/vapid/prime256v1_key.pem -encoding base64url
      pwLi7T1QqrgTiNBFBLUcndjNxzx_vZiKuCcvapwjQlM
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (pemFile == NULL) {
        Ns_TclPrintfResult(interp, "no pem file specified");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        EVP_PKEY *pkey;
        EC_KEY   *eckey = NULL;

        pkey = GetPkeyFromPem(interp, pemFile, passPhrase, NS_TRUE);
        if (pkey == NULL) {
            /*
             * GetPkeyFromPem handles error message
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
            Tcl_SetObjResult(interp, EncodedObj((unsigned char *)ds.string, octLength, NULL, encoding));

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

    Ns_Log(Debug, "import: octet length %" PRIuz, octLength);

    Tcl_DStringSetLength(dsPtr, (TCL_SIZE_T)octLength);
    octLength = EC_POINT_point2oct(EC_KEY_get0_group(eckey), ecpoint, POINT_CONVERSION_UNCOMPRESSED,
                                   (unsigned char *)dsPtr->string, octLength, bn_ctx);
    Tcl_SetObjResult(interp, EncodedObj((unsigned char *)dsPtr->string, octLength, NULL, encoding));
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
CryptoEckeyPubObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, encodingInt = -1;
    char              *pemFile = NULL,
                      *passPhrase = (char *)NS_EMPTY_STRING;
    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,   &encodingInt,binaryencodings},
        {"-passphrase", Ns_ObjvString,  &passPhrase, NULL},
        {"-pem",        Ns_ObjvString,  &pemFile,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::eckey pub -pem /usr/local/ns/modules/vapid/prime256v1_key.pem -encoding base64url
      BBGNrqwUWW4dedpYHZnoS8hzZZNMmO-i3nYButngeZ5KtJ73ZaGa00BZxke2h2RCRGm-6Rroni8tDPR_RMgNib0
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (pemFile == NULL) {
        Ns_TclPrintfResult(interp, "no pem file specified");
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        EC_KEY         *eckey;
        const EC_POINT *ecpoint = NULL;

        /*
         * The .pem file does not have a separate pub-key included,
         * but we get the pub-key grom the priv-key in form of an
         * EC_POINT.
         */
        eckey = GetEckeyFromPem(interp, pemFile, passPhrase, NS_TRUE);
        if (eckey != NULL) {
            ecpoint = EC_KEY_get0_public_key(eckey);
            if (ecpoint == NULL) {
                Ns_TclPrintfResult(interp, "no valid EC key in specified pem file");
                result = TCL_ERROR;
            } else {
                result = TCL_OK;
            }
        } else {
            result = TCL_ERROR;
        }
        if (result != TCL_ERROR) {
            Tcl_DString  ds;
            BN_CTX      *bn_ctx = BN_CTX_new();

            Tcl_DStringInit(&ds);
            SetResultFromEC_POINT(interp, &ds, eckey, ecpoint, bn_ctx, encoding);
            BN_CTX_free(bn_ctx);
            Tcl_DStringFree(&ds);
        }
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
CryptoEckeyImportObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    Tcl_Obj           *importObj = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,    &isBinary,    INT2PTR(NS_TRUE)},
        {"-string",   Ns_ObjvObj,     &importObj,   NULL},
        {"-encoding", Ns_ObjvIndex,   &encodingInt, binaryencodings},
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

    } else if (importObj == NULL) {
        Ns_TclPrintfResult(interp, "no import string specified");
        result = TCL_ERROR;

    } else {
        TCL_SIZE_T           rawKeyLength;
        const unsigned char *rawKeyString;
        EC_KEY              *eckey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        Tcl_DString          keyDs;
        Ns_BinaryEncoding    encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);

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
#  endif


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
CryptoEckeyGenerateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, nid;
    char              *curvenameString = (char *)"prime256v1", *pemFileName = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-name",     Ns_ObjvString, &curvenameString, NULL},
        {"-pem",      Ns_ObjvString, &pemFileName, NULL},
        {NULL, NULL, NULL, NULL}
    };
    /*
      ns_crypto::eckey generate -name prime256v1 -pem /tmp/foo.pem
    */

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (GetCurve(interp, curvenameString, &nid) == TCL_ERROR) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else if (pemFileName == NULL) {
        Ns_TclPrintfResult(interp, "no pem filename provided");
        result = TCL_ERROR;

    } else {
        EC_KEY       *eckey;

        eckey = EC_KEY_new_by_curve_name(nid);
        if (eckey == NULL) {
            Ns_TclPrintfResult(interp, "could not create ec key");
            result = TCL_ERROR;

        } else if (EC_KEY_generate_key(eckey) == 0) {
            Ns_TclPrintfResult(interp, "could not generate ec key");
            result = TCL_ERROR;

        } else {
            BIO        *bio;

            bio = BIO_new_file(pemFileName, "w");
            if (bio == NULL) {
                Ns_TclPrintfResult(interp, "could not open pem-file '%s' for writing", pemFileName);
                result = TCL_ERROR;
            } else {
                (void) PEM_write_bio_ECPrivateKey(bio, eckey, NULL,
                                                  NULL, 0, NULL, NULL);
                BIO_free(bio);
                result = TCL_OK;
            }
            EC_KEY_free(eckey);
        }
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
CryptoEckeySharedsecretObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, isBinary = 0, encodingInt = -1;
    char              *pemFileName = NULL,
                      *passPhrase = (char *)NS_EMPTY_STRING;
    Tcl_Obj           *pubkeyObj = NULL;
    EC_KEY            *eckey = NULL;

    Ns_ObjvSpec lopts[] = {
        {"-binary",     Ns_ObjvBool,    &isBinary,    INT2PTR(NS_TRUE)},
        {"-encoding",   Ns_ObjvIndex,   &encodingInt, binaryencodings},
        {"-passphrase", Ns_ObjvString,  &passPhrase,  NULL},
        {"-pem",        Ns_ObjvString,  &pemFileName, NULL},
        {"--",          Ns_ObjvBreak,   NULL,         NULL},
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

    } else if (pemFileName == NULL) {
        Ns_TclPrintfResult(interp, "no pem file specified");
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
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
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

#if 0
        {
            /*
             * Steps as recommended from OpenSSL wiki page. However,
             * the code based on the low-level EC_POINT_oct2point()
             * appears to be correct.
             */

            /*
             * Further ingredients:
             *
             *  pkey        : private key, from PEM, EVP_PKEY
             *  peerKeyEC   : peer key locally regenerated, same curve as pkey, get filled with octets
             *  peerKey     : peer key as EVP_PKEY, filled with peerKeyEC
             *  pctx        : parameter generation contenxt
             *  params      : parameter object
             *  kctx        : key generation context, uses params, used for EVP_PKEY_keygen_init and EVP_PKEY_keygen
             *  ctx         : shared secret generation context, uses pkey
             */
            EVP_PKEY_CTX        *pctx, *ctx = NULL, *kctx = NULL;
            EC_KEY              *peerKeyEC;
            EVP_PKEY            *peerKey, *params = NULL;
            EVP_PKEY            *pkey;

            pkey = GetPkeyFromPem(interp, pemFileName, NS_EMPTY_STRING, NS_TRUE);
            peerKeyEC = EC_KEY_new_by_curve_name(EC_GROUP_get_curve_name(group));
            peerKey = EVP_PKEY_new();

            pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
            EVP_PKEY_paramgen_init(pctx);
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, EC_GROUP_get_curve_name(group));
            Ns_Log(Notice, "NID X9_62_prime256v1 %d, privKey curve %d ", NID_X9_62_prime256v1, EC_GROUP_get_curve_name(group));

            result = TCL_ERROR;
            if (EC_KEY_oct2key(peerKeyEC, pubkeyString, (size_t)pubkeyLength, NULL) != 1) {
                Ns_Log(Notice, "could not import peer key");
                Ns_TclPrintfResult(interp, "could not import peer key");
            } else if (EVP_PKEY_set1_EC_KEY(peerKey, peerKeyEC) != 1) {
                Ns_Log(Notice, "could not convert EC key to EVP key");
                Ns_TclPrintfResult(interp, "could not convert EC key to EVP key");
            } else if (EVP_PKEY_paramgen(pctx, &params) != 1) {
                Ns_Log(Notice, "could not generate parameters");
                Ns_TclPrintfResult(interp, "could not generate parameters");
            } else if ((kctx = EVP_PKEY_CTX_new(params, NULL)) == NULL) {
                Ns_Log(Notice, "could not generate kctx");
                Ns_TclPrintfResult(interp, "could not generate kctx");
            } else if (EVP_PKEY_keygen_init(kctx) != 1) {
                Ns_Log(Notice, "could not init kctx");
                Ns_TclPrintfResult(interp, "could not init kctx");
            } else if (EVP_PKEY_keygen(kctx, &pkey) != 1) {
                Ns_Log(Notice, "could not generate key for kctx");
                Ns_TclPrintfResult(interp, "could not generate key for ctx");
            } else if ((ctx = EVP_PKEY_CTX_new(pkey, NULL)) == NULL) {
                Ns_TclPrintfResult(interp, "could not create ctx");
            } else if (EVP_PKEY_derive_init(ctx) != 1) {
                Ns_Log(Notice, "could not derive init ctx");
                Ns_TclPrintfResult(interp, "could not derive init ctx");
            } else if (EVP_PKEY_derive_set_peer(ctx, peerKey) != 1) {
                Ns_Log(Notice, "could set peer key");
                Ns_TclPrintfResult(interp, "could not set peer key");
                result = TCL_OK;
            } else {
                Tcl_DString  ds;
                size_t       sharedKeySize = 0u;

                Tcl_DStringInit(&ds);
                (void)EVP_PKEY_derive(ctx, NULL, &sharedKeySize);
                if (sharedKeySize > 0) {
                    Tcl_DStringSetLength(&ds, (TCL_SIZE_T)sharedKeySize);
                    (void)EVP_PKEY_derive(ctx, (unsigned char *)ds.string, &sharedKeySize);
                    hexPrint("recommended", (unsigned char *)ds.string, sharedKeySize);
                    result = TCL_OK;
                }
                Tcl_DStringFree(&ds);
            }

            EC_KEY_free(peerKeyEC);
            EVP_PKEY_free(peerKey);
            EVP_PKEY_free(pkey);
            EVP_PKEY_CTX_free(ctx);
            EVP_PKEY_CTX_free(pctx);
            EVP_PKEY_CTX_free(kctx);
            EVP_PKEY_free(params);
        }
#endif
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
                Tcl_SetObjResult(interp, EncodedObj((unsigned char *)ds.string, sharedSecretLength, NULL, encoding));
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
NsTclCryptoEckeyObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"generate",     CryptoEckeyGenerateObjCmd},
#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
        {"import",       CryptoEckeyImportObjCmd},
        {"priv",         CryptoEckeyPrivObjCmd},
#  endif
#  ifndef HAVE_OPENSSL_PRE_1_1
        {"sharedsecret", CryptoEckeySharedsecretObjCmd},
#  endif
        {"pub",          CryptoEckeyPubObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
# endif /* OPENSSL_NO_EC */






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
    Tcl_Interp           *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv, bool encrypt,
    Tcl_DString          *ivDsPtr, Tcl_DString *keyDsPtr, Tcl_DString *aadDsPtr,
    Tcl_DString          *tagDsPtr, Tcl_DString *inputDsPtr,
    const unsigned char **keyStringPtr,   TCL_SIZE_T *keyLengthPtr,
    const unsigned char **ivStringPtr,    TCL_SIZE_T *ivLengthPtr,
    const unsigned char **aadStringPtr,   TCL_SIZE_T *aadLengthPtr,
    char                **tagStringPtr,   TCL_SIZE_T *tagLengthPtr,
    const unsigned char **inputStringPtr, TCL_SIZE_T *inputLengthPtr,
    const EVP_CIPHER    **cipherPtr, Ns_BinaryEncoding *encodingPtr, EVP_CIPHER_CTX **ctxPtr
) {
    Tcl_Obj      *ivObj = NULL, *keyObj = NULL, *aadObj = NULL, *tagObj = NULL, *inputObj;
    int           result, isBinary = 0, encodingInt = -1;
    char         *cipherName = (char *)"aes-128-gcm";

    Ns_ObjvSpec lopts_encrypt[] = {
        {"-binary",   Ns_ObjvBool,           &isBinary,   INT2PTR(NS_TRUE)},
        {"-aad",      Ns_ObjvObj,            &aadObj,     NULL},
        {"-cipher",   Ns_ObjvString,         &cipherName, NULL},
        {"-encoding", Ns_ObjvIndex,          &encodingInt,binaryencodings},
        {"-iv",       Ns_ObjvObj,            &ivObj,      NULL},
        {"-key",      Ns_ObjvObj,            &keyObj,     NULL},
        {"--",        Ns_ObjvBreak,          NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec lopts_decrypt[] = {
        {"-binary",   Ns_ObjvBool,           &isBinary,   INT2PTR(NS_TRUE)},
        {"-aad",      Ns_ObjvObj,            &aadObj,     NULL},
        {"-cipher",   Ns_ObjvString,         &cipherName, NULL},
        {"-encoding", Ns_ObjvIndex,          &encodingInt,binaryencodings},
        {"-iv",       Ns_ObjvObj,            &ivObj,      NULL},
        {"-key",      Ns_ObjvObj,            &keyObj,     NULL},
        {"-tag",      Ns_ObjvObj,            &tagObj,     NULL},
        {"--",        Ns_ObjvBreak,          NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"input", Ns_ObjvObj, &inputObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    *ctxPtr = NULL;

    if (Ns_ParseObjv(encrypt ? lopts_encrypt : lopts_decrypt, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (keyObj == NULL) {
        Ns_TclPrintfResult(interp, "no key in specified");
        result = TCL_ERROR;

    } else if ((result = GetCipher(interp, cipherName, EVP_CIPH_GCM_MODE, "gcm", cipherPtr)) == TCL_OK) {
        *encodingPtr = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);

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
         * Get sometimes optional initialization vector (IV)
         */
        if (ivObj != NULL) {
            *ivStringPtr = Ns_GetBinaryString(ivObj, isBinary == 1, ivLengthPtr, ivDsPtr);
        } else {
            *ivStringPtr = NULL;
            *ivLengthPtr = 0;
        }

        if (tagObj != NULL) {
            *tagStringPtr = (char *)Ns_GetBinaryString(tagObj, isBinary == 1, tagLengthPtr, tagDsPtr);
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
CryptoAeadStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv, bool encrypt)
{
    int                  result;
    const EVP_CIPHER    *cipher = NULL;
    Tcl_DString          ivDs, keyDs, aadDs, tagDs, inputDs;
    Ns_BinaryEncoding    encoding = RESULT_ENCODING_HEX;
    EVP_CIPHER_CTX      *ctx;
    const unsigned char *inputString = NULL, *ivString = NULL, *aadString = NULL, *keyString = NULL;
    char                *tagString = NULL;
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
            if ((EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
                || (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)ivLength, NULL) != 1)
                || (EVP_EncryptInit_ex(ctx, NULL, NULL, keyString, ivString) != 1)
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
                    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)tagDs.length, tagDs.string);

                    listObj = Tcl_NewListObj(0, NULL);
                    /*
                     * Convert the result to the output format and return a
                     * dict containing "bytes" and "tag" as the interp result.
                     */
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("bytes", 5));
                    Tcl_ListObjAppendElement(interp, listObj, EncodedObj((unsigned char *)outputDs.string,
                                                                         (size_t)outputDs.length,
                                                                         NULL, encoding));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("tag", 3));
                    Tcl_ListObjAppendElement(interp, listObj, EncodedObj((unsigned char *)tagDs.string,
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

            if (tagString == NULL) {
                Ns_TclPrintfResult(interp, "option '-tag' has to be provided for decryption");
                result = TCL_ERROR;

            } else if ((EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
                       || (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)ivLength, NULL) != 1)
                       || (EVP_DecryptInit_ex(ctx, NULL, NULL, keyString, ivString) != 1)
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

                    /*
                     * Set expected tag value. Works in OpenSSL 1.0.1d and later
                     */
                    if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)tagLength, tagString) != 1) {
                        Ns_TclPrintfResult(interp, "could not set tag value");
                        result = TCL_ERROR;
                    } else {

                        (void)EVP_DecryptFinal_ex(ctx,
                                                  (unsigned char  *)(outputDs.string + length),
                                                  &length);
                        outputLength += (TCL_SIZE_T)length;
                        //fprintf(stderr, "allocated size %d, final size %d\n", inputLength, outputLength);
                        Tcl_DStringSetLength(&outputDs, outputLength);
                        Tcl_SetObjResult(interp, EncodedObj((unsigned char *)outputDs.string,
                                                            (size_t)outputDs.length,
                                                            NULL, encoding));
                    }
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
CryptoAeadEncryptStringObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    return CryptoAeadStringObjCmd(clientData, interp, objc, objv, NS_TRUE);
}
static int
CryptoAeadDecryptStringObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    return CryptoAeadStringObjCmd(clientData, interp, objc, objv, NS_FALSE);
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoAeadEncryptObjCmd, NsTclCryptoAeadDecryptObjCmd --
 *
 *      Implements "ns_crypto::aead::encrypt" and
 *      "ns_crypto::aead::dncrypt". Returns encrypted/decrypted data.
 *
 * Results:
 *      NS_OK
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoAeadEncryptObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoAeadEncryptStringObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
int
NsTclCryptoAeadDecryptObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoAeadDecryptStringObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

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
NsTclCryptoRandomBytesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int                result, nrBytes = 0, encodingInt = -1;
    Ns_ObjvValueRange  lengthRange = {1, INT_MAX};
    Ns_ObjvSpec lopts[] = {
        {"-encoding",   Ns_ObjvIndex,   &encodingInt, binaryencodings},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"bytes", Ns_ObjvInt, &nrBytes, &lengthRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_BinaryEncoding encoding = (encodingInt == -1 ? RESULT_ENCODING_HEX : (Ns_BinaryEncoding)encodingInt);
        Tcl_DString ds;
        int         rc;

        Tcl_DStringInit(&ds);
        Tcl_DStringSetLength(&ds, (TCL_SIZE_T)nrBytes);
        rc = RAND_bytes((unsigned char *)ds.string, nrBytes);
        if (likely(rc == 1)) {
            Tcl_SetObjResult(interp, EncodedObj((unsigned char *)ds.string, (size_t)nrBytes, NULL, encoding));
            result = TCL_OK;
        } else {
            Ns_TclPrintfResult(interp, "could not obtain random bytes from OpenSSL");
            result = TCL_ERROR;
        }
        Tcl_DStringFree(&ds);
    }

    return result;
}

# ifdef OPENSSL_NO_EC
int
NsTclCryptoEckeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "The used version of OpenSSL was built without EC support");
    return TCL_ERROR;
}
# endif

#else
/*
 * Compile without OpenSSL support or too old OpenSSL versions
 */

int
NsTclCryptoHmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoMdObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoAeadDecryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}
int
NsTclCryptoAeadEncryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoRandomBytesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoEckeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoScryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.0 built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoArgon2ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T UNUSED(ojbc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.2 built into NaviServer");
    return TCL_ERROR;
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
