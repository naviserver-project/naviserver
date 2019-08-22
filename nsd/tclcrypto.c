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
 * tclcrypto.c --
 *
 *      Function callable from Tcl to use OpenSSL crypto support
 */

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

# ifndef HAVE_OPENSSL_PRE_1_1
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
} Ns_ResultEncoding;

/*
 * Static functions defined in this file.
 */
static Tcl_Obj *EncodedObj(
    unsigned char *octects,
    size_t octectLength,
    char *outputBuffer,
    Ns_ResultEncoding encoding
) NS_GNUC_NONNULL(1);

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
    Ns_ResultEncoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
# endif /* OPENSSL_NO_EC */

static int GetCipher(
  Tcl_Interp *interp, const char *cipherName, unsigned long flags,
  const char *modeMsg, const EVP_CIPHER **cipherPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

# ifndef HAVE_OPENSSL_PRE_1_0
static void ListMDfunc(const EVP_MD *m, const char *from, const char *to, void *arg);
# endif

static Tcl_ObjCmdProc CryptoHmacAddObjCmd;
static Tcl_ObjCmdProc CryptoHmacFreeObjCmd;
static Tcl_ObjCmdProc CryptoHmacGetObjCmd;
static Tcl_ObjCmdProc CryptoHmacNewObjCmd;
static Tcl_ObjCmdProc CryptoHmacStringObjCmd;

static Tcl_ObjCmdProc CryptoMdAddObjCmd;
static Tcl_ObjCmdProc CryptoMdFreeObjCmd;
static Tcl_ObjCmdProc CryptoMdGetObjCmd;
static Tcl_ObjCmdProc CryptoMdNewObjCmd;
static Tcl_ObjCmdProc CryptoMdStringObjCmd;

# ifndef OPENSSL_NO_EC
#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
static Tcl_ObjCmdProc CryptoEckeyPrivObjCmd;
static Tcl_ObjCmdProc CryptoEckeyImportObjCmd;
#  endif
# endif

/*
 * Local variables defined in this file.
 */

static const char * const mdCtxType  = "ns:mdctx";
static const char * const hmacCtxType  = "ns:hmacctx";

static Ns_ObjvValueRange posIntRange0 = {0, INT_MAX};
# ifdef HAVE_OPENSSL_3
static Ns_ObjvValueRange posIntRange1 = {1, INT_MAX};
# endif

/*
 *----------------------------------------------------------------------
 *
 * Debug function to ease work with binary data.
 *
 *----------------------------------------------------------------------
 */
static void hexPrint(const char *msg, const unsigned char *octects, size_t octectLength)
{
    if (Ns_LogSeverityEnabled(Debug)) {
        size_t i;
        Tcl_DString ds;

        Tcl_DStringInit(&ds);;
        Ns_DStringPrintf(&ds, "%s (len %zu): ", msg, octectLength);
        for (i=0; i<octectLength; i++) {
            Ns_DStringPrintf(&ds, "%.2x ",octects[i] & 0xff);
         }
         Ns_Log(Debug, "%s", ds.string);
         Tcl_DStringFree(&ds);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * GetResultEncoding, EncodedObj --
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

static int
GetResultEncoding(Tcl_Interp *interp, const char *name, Ns_ResultEncoding *encodingPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    NS_NONNULL_ASSERT(encodingPtr != NULL);

    if (strcmp(name, "hex") == 0) {
        *encodingPtr = RESULT_ENCODING_HEX;
    } else if (strcmp(name, "base64url") == 0) {
        *encodingPtr = RESULT_ENCODING_BASE64URL;
    } else if (strcmp(name, "base64") == 0) {
        *encodingPtr = RESULT_ENCODING_BASE64;
    } else if (strcmp(name, "binary") == 0) {
        *encodingPtr = RESULT_ENCODING_BINARY;
    } else {
        Ns_TclPrintfResult(interp, "Unknown value for output encoding \"%s\", "
                           "valid: hex, base64url, base64, binary",
                           name);
        result = TCL_ERROR;
    }
    return result;
}

static Tcl_Obj*
EncodedObj(unsigned char *octects, size_t octectLength,
              char *outputBuffer, Ns_ResultEncoding encoding) {
    char    *origOutputBuffer = outputBuffer;
    Tcl_Obj *resultObj;

    NS_NONNULL_ASSERT(octects != NULL);

    if (outputBuffer == NULL && encoding != RESULT_ENCODING_BINARY) {
        /*
         * It is a safe assumption to double the size, since the hex
         * encoding needs to most space.
         */
        outputBuffer = ns_malloc(octectLength * 2u + 1u);
    }

    switch (encoding) {
    case RESULT_ENCODING_BINARY:
        resultObj = Tcl_NewByteArrayObj((const unsigned char *)octects, (int)octectLength);
        break;

    case RESULT_ENCODING_BASE64URL:
        hexPrint("result", octects, octectLength);
        (void)Ns_HtuuEncode2(octects, octectLength, outputBuffer, 1);
        resultObj = Tcl_NewStringObj(outputBuffer, (int)strlen(outputBuffer));
        break;

    case RESULT_ENCODING_BASE64:
        (void)Ns_HtuuEncode2(octects, octectLength, outputBuffer, 0);
        resultObj = Tcl_NewStringObj(outputBuffer, (int)strlen(outputBuffer));
        break;

    case RESULT_ENCODING_HEX:
        Ns_HexString(octects, outputBuffer, (int)octectLength, NS_FALSE);
        resultObj = Tcl_NewStringObj(outputBuffer, (int)octectLength*2);
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
 *      representation.  ListMDfunc is a iterator usabel in OpenSSL
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

        /* fprintf(stderr, "from %s to %to name <%s> type (nid) %d\n",from,to,mdName, EVP_MD_type(m)); */
        /*
         * Apprarently, the list contains upper and lower case variants. Avoid
         * duplication.
         */
        if ((*from >= 'a') && (*from <= 'z')) {
            (void)Tcl_ListObjAppendElement(NULL, listPtr, Tcl_NewStringObj(mdName, -1));
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
 * GetPkeyFromPem, GetEckeyFromPem, password_callback --
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
typedef struct PW_CB_DATA {
    const void *password;
} PW_CB_DATA;

static int
password_callback(char *UNUSED(buf), int bufsiz, int UNUSED(verify), PW_CB_DATA *UNUSED(cb_tmp))
{
    int result = 0;
    //PW_CB_DATA *cb_data = (PW_CB_DATA *)cb_tmp;

    fprintf(stderr, "password_callback called with bufsize %d\n", bufsiz);

    return result;
}


static EVP_PKEY *
GetPkeyFromPem(Tcl_Interp *interp, char *pemFileName, bool private)
{
    BIO        *bio;
    EVP_PKEY   *result;
    PW_CB_DATA  cb_data;

    cb_data.password = NS_EMPTY_STRING;
    bio = BIO_new_file(pemFileName, "r");
    if (bio == NULL) {
        Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", pemFileName);
        result = NULL;
    } else {
        if (private) {
            result = PEM_read_bio_PrivateKey(bio, NULL,
                                             (pem_password_cb *)password_callback,
                                             &cb_data);
        } else {
            result = PEM_read_bio_PUBKEY(bio, NULL,
                                         (pem_password_cb *)password_callback,
                                         &cb_data);
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
GetEckeyFromPem(Tcl_Interp *interp, char *pemFileName, bool private)
{
    BIO        *bio;
    EC_KEY     *result;
    PW_CB_DATA  cb_data;

    cb_data.password = NS_EMPTY_STRING;
    bio = BIO_new_file(pemFileName, "r");
    if (bio == NULL) {
        Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", pemFileName);
        result = NULL;
    } else {
        if (private) {
            result = PEM_read_bio_ECPrivateKey(bio, NULL,
                                           (pem_password_cb *)password_callback,
                                           &cb_data);
        } else {
            result = PEM_read_bio_EC_PUBKEY(bio, NULL,
                                         (pem_password_cb *)password_callback,
                                         &cb_data);
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
CryptoHmacNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int            result;
    const char    *digestName = "sha256";
    Tcl_Obj       *keyObj;
    Ns_ObjvSpec    args[] = {
        {"digest",  Ns_ObjvString, &digestName, NULL},
        {"key",     Ns_ObjvObj,    &keyObj, NULL},
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
            HMAC_CTX   *ctx;
            const char *keyString;
            int         keyLength;
            Tcl_DString keyDs;

            Tcl_DStringInit(&keyDs);
            keyString = Ns_GetBinaryString(keyObj, NS_FALSE, &keyLength, &keyDs);
            ctx = HMAC_CTX_new();
            HMAC_Init_ex(ctx, keyString, keyLength, md, NULL);
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
 *        Incremental command to add a message chunk to a predefined
 *        HMAC context, which was previously created via the "new"
 *        subcommand.
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
CryptoHmacAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int            result = TCL_OK;
    HMAC_CTX      *ctx;
    const Tcl_Obj *ctxObj;
    Tcl_Obj       *messageObj;
    int            messageLength;
    Ns_ObjvSpec    args[] = {
        {"ctx",      Ns_ObjvObj, &ctxObj, NULL},
        {"message",  Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, hmacCtxType, (void **)&ctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", hmacCtxType);
        result = TCL_ERROR;

    } else {
        const unsigned char *message;
        Tcl_DString          messageDs;

        Tcl_DStringInit(&messageDs);
        message = (const unsigned char *)Ns_GetBinaryString(messageObj, NS_FALSE, &messageLength, &messageDs);
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
 *        Incremental command to get the (maybe partial) HMAC result
 *        in form of a hex string.
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
CryptoHmacGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK;
    HMAC_CTX          *ctx;
    const Tcl_Obj     *ctxObj;
    char              *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec    lopts[] = {
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
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
 *        Free a previously allocated HMAC context.
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
CryptoHmacFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
 *        Single command to obtain an HMAC from the provided data.
 *        Technically, this is a combination of the other subcommands,
 *        but requires that the all data for the HMAC computation is
 *        provided in the contents of a Tcl_Obj in memory. The command
 *        returns the HMAC in form of a hex string.
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
CryptoHmacStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result;
    Tcl_Obj           *keyObj, *messageObj;
    const char        *digestName = "sha256";
    char              *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec    lopts[] = {
        {"-digest",   Ns_ObjvString, &digestName, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {"--",        Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"key",     Ns_ObjvObj, &keyObj, NULL},
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        const EVP_MD  *md;

        /*
         * Look up the Message digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {
            unsigned char  digest[EVP_MAX_MD_SIZE];
            char           digestChars[EVP_MAX_MD_SIZE*2 + 1];
            HMAC_CTX      *ctx;
            const char    *keyString, *messageString;
            unsigned int   mdLength;
            int            keyLength, messageLength;
            Tcl_DString    keyDs, messageDs;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&keyDs);
            Tcl_DStringInit(&messageDs);
            keyString = Ns_GetBinaryString(keyObj, NS_FALSE, &keyLength, &keyDs);
            messageString = Ns_GetBinaryString(messageObj, NS_FALSE, &messageLength, &messageDs);
            hexPrint("hmac key", (const unsigned char *)keyString, (size_t)keyLength);
            hexPrint("hmac message", (const unsigned char *)messageString, (size_t)messageLength);

            /*
             * Call the HMAC computation.
             */
            ctx = HMAC_CTX_new();
            HMAC(md,
                 (const void *)keyString, keyLength,
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
 *      Various subcmds for handling Hash-based message authentications codes
 *      (HMAC)
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
NsTclCryptoHmacObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
 *        Incremental command to initialize a MD context. This
 *        command is typically followed by a sequence of "add"
 *        subcommands until the content is read with the "get"
 *        subcommand and then freed.
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
CryptoMdNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int            result;
    const char    *digestName = "sha256";
    Ns_ObjvSpec    args[] = {
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
 *        Incremental command to add a message chunk to a predefined
 *        MD context, which was previously created via the "new"
 *        subcommand.
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
CryptoMdAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int            result = TCL_OK;
    EVP_MD_CTX    *mdctx;
    const Tcl_Obj *ctxObj;
    Tcl_Obj       *messageObj;
    Ns_ObjvSpec    args[] = {
        {"ctx",      Ns_ObjvObj, &ctxObj, NULL},
        {"message",  Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, mdCtxType, (void **)&mdctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", mdCtxType);
        result = TCL_ERROR;

    } else {
        const char    *message;
        int            messageLength;
        Tcl_DString    messageDs;

        Tcl_DStringInit(&messageDs);
        message = Ns_GetBinaryString(messageObj, NS_FALSE, &messageLength, &messageDs);
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
 *        Incremental command to get the (maybe partial) MD result in
 *        form of a hex string.
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
CryptoMdGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK;
    EVP_MD_CTX        *mdctx;
    const Tcl_Obj     *ctxObj;
    char              *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
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
 *        Free a previously allocated MD context.
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
CryptoMdFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
 *        Single command to obtain a MD (message digest) from the
 *        provided data.  Technically, this is a combination of the
 *        other subcommands, but requires that the all data for the MD
 *        computation is provided in the contents of a Tcl_Obj in
 *        memory. The command returns the MD in form of a hex string.
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
CryptoMdStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result;
    Tcl_Obj           *messageObj;
    char              *digestName = (char *)"sha256", *keyFile = NULL, *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-digest",   Ns_ObjvString, &digestName, NULL},
        {"-sign",     Ns_ObjvString, &keyFile,    NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {"--",        Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        const EVP_MD *md;
        EVP_PKEY     *pkey = NULL;

        /*
         * Look up the Message Digest from OpenSSL
         *
         * ::ns_crypto::md string -digest sha256 -sign /usr/local/src/naviserver/private.pem "hello\n"
         *
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR && keyFile != NULL) {
#if 0
            sigkey  = load_key(keyFile, OPT_FMT_ANY, 0,
                               NULL /*pass phrase*/,
                               NULL /*engine, maybe hardware*/,
                               "key file");
            key = bio_open_default(file, 'r', format);

#endif
            pkey = GetPkeyFromPem(interp, keyFile, NS_TRUE);
            if (pkey == NULL) {
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR) {
            unsigned char  digest[EVP_MAX_MD_SIZE];
            char           digestChars[EVP_MAX_MD_SIZE*2 + 1], *outputBuffer = digestChars;
            EVP_MD_CTX    *mdctx;
            const char    *messageString;
            int            messageLength;
            unsigned int   mdLength;
            Tcl_DString    messageDs;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&messageDs);
            messageString = Ns_GetBinaryString(messageObj, NS_FALSE, &messageLength, &messageDs);
            hexPrint("md", (const unsigned char *)messageString, (size_t)messageLength);

            /*
             * Call the Digest or Signature computation
             */
            mdctx = NS_EVP_MD_CTX_new();
            if (pkey != NULL) {
                EVP_PKEY_CTX  *pctx;
                int            r = EVP_DigestSignInit(mdctx, &pctx, md, NULL /*engine*/, pkey);

                if (r == 0) {
                    Ns_TclPrintfResult(interp, "could not initialize signature context");
                    result = TCL_ERROR;
                    pctx = NULL;
                    mdLength = 0u;
                } else {
                    size_t mdSize;

                    (void)EVP_DigestSignUpdate(mdctx, messageString, (size_t)messageLength);
                    (void)EVP_DigestSignFinal(mdctx, digest, &mdSize);
                    //fprintf(stderr, "final signature length %u\n",mdLength);
                    outputBuffer = ns_malloc(mdSize * 2u + 1u);
                    mdLength = (unsigned int)mdSize;
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

            NS_EVP_MD_CTX_free(mdctx);

            if (result == TCL_OK) {
                /*
                 * Convert the result to the output format and set the interp
                 * result.
                 */
                Tcl_SetObjResult(interp, EncodedObj(digest, mdLength, outputBuffer, encoding));
            }
            if (outputBuffer != digestChars) {
                ns_free(outputBuffer);
            }
            Tcl_DStringFree(&messageDs);
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
 *        Subcommand to sign a message according to the
 *        Voluntary Application Server Identification (VAPID) for Web Push
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
CryptoMdVapidSignObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result;
    Tcl_Obj           *messageObj;
    char              *digestName = (char *)"sha256", *pemFile = NULL, *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-digest",   Ns_ObjvString, &digestName, NULL},
        {"-pem",      Ns_ObjvString, &pemFile,    NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {"--",        Ns_ObjvBreak,  NULL,        NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        const EVP_MD *md;
        EC_KEY       *eckey = NULL;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {

            eckey = GetEckeyFromPem(interp, pemFile, NS_TRUE);
            if (eckey == NULL) {
                /*
                 * GetEckeyFromPem handles error message
                 */
                result = TCL_ERROR;
            }
        }
        if (result != TCL_ERROR) {
            unsigned char  digest[EVP_MAX_MD_SIZE];
            EVP_MD_CTX    *mdctx;
            const char    *messageString;
            int            messageLength;
            unsigned int   sigLen, mdLength, rLen, sLen;
            Tcl_DString    messageDs;
            ECDSA_SIG     *sig;
            const BIGNUM  *r, *s;
            uint8_t       *rawSig;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&messageDs);
            messageString = Ns_GetBinaryString(messageObj, NS_FALSE, &messageLength, &messageDs);

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
 *        Subcommand of ns_crypto::md to derive keys based on message digests.
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
CryptoMdHkdfObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result, outLength = 0;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL, *infoObj = NULL;
    char              *digestName = (char *)"sha256", *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;
    Ns_ObjvSpec lopts[] = {
        {"-digest",   Ns_ObjvString, &digestName, NULL},
        {"-salt",     Ns_ObjvObj,    &saltObj,    NULL},
        {"-secret",   Ns_ObjvObj,    &secretObj,  NULL},
        {"-info",     Ns_ObjvObj,    &infoObj,    NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {"--",        Ns_ObjvBreak,  NULL,        NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
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
            const char    *infoString, *saltString, *secretString;
            unsigned char *keyString;
            Tcl_DString    infoDs, saltDs, secretDs;
            int            infoLength, saltLength, secretLength;
            size_t         outSize = (size_t)outLength;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&saltDs);
            Tcl_DStringInit(&secretDs);
            Tcl_DStringInit(&infoDs);
            keyString = ns_malloc((size_t)outLength);

            saltString   = Ns_GetBinaryString(saltObj,   NS_FALSE, &saltLength,   &saltDs);
            secretString = Ns_GetBinaryString(secretObj, NS_FALSE, &secretLength, &secretDs);
            infoString   = Ns_GetBinaryString(infoObj,   NS_FALSE, &infoLength,   &infoDs);

            // hexPrint("salt  ", (const unsigned char *)saltString,   (size_t)saltLength);
            // hexPrint("secret", (const unsigned char *)secretString, (size_t)secretLength);
            // hexPrint("info  ", (const unsigned char *)infoString,   (size_t)infoLength);

            if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, saltString, saltLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set salt");
                result = TCL_ERROR;
            } else if (EVP_PKEY_CTX_set1_hkdf_key(pctx, secretString, secretLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set secret");
                result = TCL_ERROR;
            } else if (EVP_PKEY_CTX_add1_hkdf_info(pctx, infoString, infoLength) <= 0) {
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
 *      Returns a Hash-based message authentication code of the provided message
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
NsTclCryptoMdObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
 *----------------------------------------------------------------------
 *
 * NsCryptoScryptObjCmd --
 *
 *      Compute a "password hash" using the scrypt Password-Based
 *      Key Derivation Function (RFC 7914) as defined in OpenSSL 3
 *
 *      Implementation of ::ns_crypto::scrypt
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
NsCryptoScryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result, n = 1024, r = 8, p = 16;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL;
    char              *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;
    Ns_ObjvSpec lopts[] = {
        {"-salt",     Ns_ObjvObj,    &saltObj,    NULL},
        {"-secret",   Ns_ObjvObj,    &secretObj,  NULL},
        {"-n",        Ns_ObjvInt,    &n,          &posIntRange1},
        {"-p",        Ns_ObjvInt,    &p,          &posIntRange1},
        {"-r",        Ns_ObjvInt,    &r,          &posIntRange1},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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
      ::ns_crypto::scrypt -secret "pleaseletmein" -salt SodiumChloride -n 16384 -r 8 -p 1

      7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2
      d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887

      % time {::ns_crypto::scrypt -secret "pleaseletmein" -salt SodiumChloride -n 16384 -r 8 -p 1}
      47901 microseconds per iteration

      ############################################################################
      # Test Case 3: RFC 7914 (example 4 in sect 12)
      ############################################################################
      ::ns_crypto::scrypt -secret "pleaseletmein" -salt SodiumChloride -n 1048576 -r 8 -p 1

      2101cb9b6a511aaeaddbbe09cf70f881ec568d574a2ffd4dabe5ee9820adaa47
      8e56fd8f4ba5d09ffa1c6d927c40f4c337304049e8a952fbcbf45c6fa77a41a4

      % time {::ns_crypto::scrypt -secret "pleaseletmein" -salt SodiumChloride -n 1048576 -r 8 -p 1}
      3095741 microseconds per iteration
    */

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else if (saltObj == NULL) {
        Ns_TclPrintfResult(interp, "no -salt specified");
        result = TCL_ERROR;

    } else if (secretObj == NULL) {
        Ns_TclPrintfResult(interp, "no -secret specified");
        result = TCL_ERROR;

    } else {

        EVP_KDF_CTX  *kctx;
        unsigned char out[64];
        Tcl_DString    saltDs, secretDs;
        int            saltLength, secretLength;
        const char    *saltString, *secretString;

        /*
         * All input parameters are valid, get key and data.
         */
        Tcl_DStringInit(&saltDs);
        Tcl_DStringInit(&secretDs);
        //keyString = ns_malloc((size_t)outLength);

        saltString   = Ns_GetBinaryString(saltObj,   NS_FALSE, &saltLength,   &saltDs);
        secretString = Ns_GetBinaryString(secretObj, NS_FALSE, &secretLength, &secretDs);

        kctx = EVP_KDF_CTX_new_id(EVP_KDF_SCRYPT);

        if (EVP_KDF_ctrl(kctx, EVP_KDF_CTRL_SET_PASS, secretString, (size_t)secretLength) <= 0) {
            Ns_TclPrintfResult(interp, "could not set secret");
            result = TCL_ERROR;

        } else if (EVP_KDF_ctrl(kctx, EVP_KDF_CTRL_SET_SALT, saltString, (size_t)saltLength) <= 0) {
            Ns_TclPrintfResult(interp, "could not set salt");
            result = TCL_ERROR;

        } else if (EVP_KDF_ctrl(kctx, EVP_KDF_CTRL_SET_SCRYPT_N, (uint64_t)n) <= 0) {
            Ns_TclPrintfResult(interp, "could not set scrypt N (work factor, positive power of 2)");
            result = TCL_ERROR;

        } else if (EVP_KDF_ctrl(kctx, EVP_KDF_CTRL_SET_SCRYPT_R, (uint32_t)r) <= 0) {
            Ns_TclPrintfResult(interp, "could not set scrypt r (block size)");
            result = TCL_ERROR;

        } else if (EVP_KDF_ctrl(kctx, EVP_KDF_CTRL_SET_SCRYPT_P, (uint32_t)p) <= 0) {
            Ns_TclPrintfResult(interp, "could not set scrypt p (parallelization function)");
            result = TCL_ERROR;

        } else if (EVP_KDF_derive(kctx, out, sizeof(out)) <= 0) {
            Ns_TclPrintfResult(interp, "could not derive scrypt value from parameters");
            result = TCL_ERROR;

        } else {
            /*
             * Convert the result to the output format and set the interp
             * result.
             */
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
NsCryptoScryptObjCmd (ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.0 built into NaviServer");
    return TCL_ERROR;
}
# endif



# ifndef OPENSSL_NO_EC
#  ifdef HAVE_OPENSSL_EC_PRIV2OCT
/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeyPrivObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Subcommand of ns_crypto::eckey to obtain the private key in
 *        various encodings from an elliptic curves PEM file.
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
CryptoEckeyPrivObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result;
    char              *pemFile = NULL, *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-pem",      Ns_ObjvString, &pemFile, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        EVP_PKEY *pkey;
        EC_KEY   *eckey = NULL;

        pkey = GetPkeyFromPem(interp, pemFile, NS_TRUE);
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
            Tcl_DStringSetLength(&ds, (int)octLength);
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
    Ns_ResultEncoding encoding)
{
    size_t   octLength = EC_POINT_point2oct(EC_KEY_get0_group(eckey), ecpoint,
                                            POINT_CONVERSION_UNCOMPRESSED, NULL, 0, NULL);

    Ns_Log(Notice, "import: octet length %" PRIuz, octLength);

    Tcl_DStringSetLength(dsPtr, (int)octLength);
    octLength = EC_POINT_point2oct(EC_KEY_get0_group(eckey), ecpoint, POINT_CONVERSION_UNCOMPRESSED,
                                   (unsigned char *)dsPtr->string, octLength, bn_ctx);
    Tcl_SetObjResult(interp, EncodedObj((unsigned char *)dsPtr->string, octLength, NULL, encoding));
}


/*
 *----------------------------------------------------------------------
 *
 * CryptoEckeyPubObjCmd -- Subcommand of NsTclCryptoEckeyObjCmd
 *
 *        Subcommand of ns_crypto::eckey to obtain the public key in
 *        various encodings from an elliptic curves PEM file.
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
CryptoEckeyPubObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result;
    char              *pemFile = NULL, *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-pem",      Ns_ObjvString, &pemFile, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        EC_KEY         *eckey;
        const EC_POINT *ecpoint = NULL;

        /*
         * The .pem file does not have a separate pub-key included,
         * but we get the pub-key grom the priv-key in form of an
         * EC_POINT.
         */
        eckey = GetEckeyFromPem(interp, pemFile, NS_TRUE);
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
 *        Subcommand of ns_crypto::eckey to import a public key
 *        into the OpenSSL EC_KEY structure in order to apply
 *        conversions of it. Can be most likely dropped.
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
CryptoEckeyImportObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result;
    char              *outputEncodingString = NULL;
    Tcl_Obj           *importObj = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-string",   Ns_ObjvObj,    &importObj, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        int                  rawKeyLength;
        const unsigned char *rawKeyString;
        EC_KEY              *eckey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        Tcl_DString          keyDs;

        Tcl_DStringInit(&keyDs);
        rawKeyString = (const unsigned char *)Ns_GetBinaryString(importObj, NS_FALSE, &rawKeyLength, &keyDs);

        Ns_Log(Notice, "import: raw key length %d", rawKeyLength);
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
 *        Subcommand of ns_crypto::eckey to generate an EC pemfile
 *        without the need of an external command.
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
CryptoEckeyGenerateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
        Ns_TclPrintfResult(interp, "no pem file name provided");
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
 *        Subcommand of ns_crypto::eckey to generate a shared secret
 *        based on the private key from the .pem file and the provided
 *        public key.
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
CryptoEckeySharedsecretObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result;
    char              *outputEncodingString = NULL, *pemFileName = NULL;
    Tcl_Obj           *pubkeyObj = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;
    EC_KEY            *eckey = NULL;

    Ns_ObjvSpec lopts[] = {
        {"-pem",      Ns_ObjvString, &pemFileName, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {"--",        Ns_ObjvBreak,  NULL,         NULL},
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

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {

        eckey = GetEckeyFromPem(interp, pemFileName, NS_TRUE);
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
        int                  pubkeyLength;
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
        pubkeyString = (const unsigned char *)Ns_GetBinaryString(pubkeyObj, NS_FALSE, &pubkeyLength, &importDs);

        //pubkeyString = Tcl_GetByteArrayFromObj(pubkeyObj, &pubkeyLength);
        //Ns_Log(Notice, "pub key length %d", pubkeyLength);

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
             *  peerKeyEC   : peer key locally regnerated, same curve as pkey, get filled with octets
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

            pkey = GetPkeyFromPem(interp, pemFileName, NS_TRUE);
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
                    Tcl_DStringSetLength(&ds, (int)sharedKeySize);
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
            Tcl_DStringSetLength(&ds, (int)sharedSecretLength);

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
 *      Provide subcommands to handle EC (elliptic curve) cryptography
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
NsTclCryptoEckeyObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
    Tcl_Interp        *interp, int objc, Tcl_Obj *const* objv, bool encrypt,
    Tcl_Obj          **tagObjPtr,
    Tcl_DString       *ivDsPtr, Tcl_DString *keyDsPtr, Tcl_DString *aadDsPtr,
    const char       **keyStringPtr, int *keyLengthPtr,
    const char       **ivStringPtr, int *ivLengthPtr,
    const char       **aadStringPtr, int *aadLengthPtr,
    const char       **inputStringPtr,  int *inputLengthPtr,
    const EVP_CIPHER **cipherPtr, Ns_ResultEncoding *encodingPtr, EVP_CIPHER_CTX **ctxPtr
) {
    Tcl_Obj      *ivObj = NULL, *keyObj = NULL, *aadObj = NULL, *inputObj;
    int           result;
    char         *cipherName = (char *)"aes-128-gcm";
    Tcl_DString   ivDs, inputDs;
    char         *outputEncodingString = NULL;

    Ns_ObjvSpec lopts_encrypt[] = {
        {"-aad",      Ns_ObjvObj,     &aadObj,     NULL},
        {"-cipher",   Ns_ObjvString,  &cipherName, NULL},
        {"-encoding", Ns_ObjvString,  &outputEncodingString, NULL},
        {"-iv",       Ns_ObjvObj,     &ivObj,      NULL},
        {"-key",      Ns_ObjvObj,     &keyObj,     NULL},
        {"--",        Ns_ObjvBreak,   NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec lopts_decrypt[] = {
        {"-aad",      Ns_ObjvObj,     &aadObj,     NULL},
        {"-cipher",   Ns_ObjvString,  &cipherName, NULL},
        {"-encoding", Ns_ObjvString,  &outputEncodingString, NULL},
        {"-iv",       Ns_ObjvObj,     &ivObj,      NULL},
        {"-key",      Ns_ObjvObj,     &keyObj,     NULL},
        {"-tag",      Ns_ObjvObj,     tagObjPtr,   NULL},
        {"--",        Ns_ObjvBreak,   NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"input", Ns_ObjvObj, &inputObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    *tagObjPtr = NULL;
    *encodingPtr = RESULT_ENCODING_HEX;
    *ctxPtr = NULL;

    Tcl_DStringInit(&ivDs);

    if (Ns_ParseObjv(encrypt ? lopts_encrypt : lopts_decrypt, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (keyObj == NULL) {
        Ns_TclPrintfResult(interp, "no key in specified");
        result = TCL_ERROR;

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, encodingPtr) != TCL_OK) {
        /*
         * Function cares about error message.
         */
        result = TCL_ERROR;

    } else if ((result = GetCipher(interp, cipherName, EVP_CIPH_GCM_MODE, "gcm", cipherPtr)) == TCL_OK) {

        *ctxPtr = EVP_CIPHER_CTX_new();
        *keyStringPtr = Ns_GetBinaryString(keyObj, NS_FALSE, keyLengthPtr, keyDsPtr);

        /*
         * Get optional additional authenticated data (AAD)
         */
        if (aadObj != NULL) {
            *aadStringPtr = Ns_GetBinaryString(aadObj, NS_FALSE, aadLengthPtr, aadDsPtr);
        } else {
            *aadLengthPtr = 0;
            *aadStringPtr = NS_EMPTY_STRING;
        }

        /*
         * Get sometimes optional initialization vector (IV)
         */
        if (ivObj != NULL) {
            *ivStringPtr = Ns_GetBinaryString(ivObj, NS_FALSE, ivLengthPtr, ivDsPtr);
        } else {
            *ivStringPtr = NULL;
            *ivLengthPtr = 0;
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
            *inputStringPtr = Ns_GetBinaryString(inputObj, NS_FALSE, inputLengthPtr, &inputDs);
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
 *        Sub command of NsTclCryptoAeadObjCmd to encrypt or decrypt
 *        string data. Encryption returns a dict with "bytes" and the
 *        "tag" necessary for decoding.
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
CryptoAeadStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, bool encrypt)
{
    int                  result;
    Tcl_Obj             *tagObj = NULL;
    const EVP_CIPHER    *cipher = NULL;
    Tcl_DString          ivDs, keyDs, aadDs, inputDs;
    Ns_ResultEncoding    encoding = RESULT_ENCODING_HEX;
    EVP_CIPHER_CTX      *ctx;
    const char          *inputString = NULL, *ivString, *aadString, *keyString = NULL;
    int                  inputLength, keyLength, ivLength, aadLength;

    /*
      ::ns_crypto::aead::encrypt string -cipher aes-128-gcm -iv 123456789 -key secret "hello world"

      set d [::ns_crypto::aead::encrypt string -cipher aes-128-gcm -iv 123456789 -key secret -encoding binary "hello world"]
      ns_crypto::aead::decrypt string -cipher aes-128-gcm -iv 123456789 -key secret -tag [dict get $d tag] -encoding binary [dict get $d bytes]

    */

    Tcl_DStringInit(&inputDs);
    Tcl_DStringInit(&aadDs);
    Tcl_DStringInit(&keyDs);
    Tcl_DStringInit(&ivDs);

    result = CryptoAeadStringGetArguments(interp, objc, objv, encrypt,
                                          &tagObj, &ivDs, &keyDs, &aadDs,
                                          &keyString,   &keyLength,
                                          &ivString,    &ivLength,
                                          &aadString,   &aadLength,
                                          &inputString, &inputLength,
                                          &cipher, &encoding, &ctx);
    if (result == TCL_OK) {
        int length;

        if (encrypt) {
            /*
             * Encrypt ...
             */
            if ((EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
                || (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, ivLength, NULL) != 1)
                || (EVP_EncryptInit_ex(ctx, NULL, NULL, (const unsigned char *)keyString, (const unsigned char *)ivString) != 1)
                ) {
                Ns_TclPrintfResult(interp, "could not initialize encryption context");
                result = TCL_ERROR;

            } else if (EVP_EncryptUpdate(ctx, NULL, &length, (const unsigned char *)aadString, aadLength) != 1) {
                /*
                 * To specify additional authenticated data (AAD), a call
                 * to EVP_CipherUpdate(), EVP_EncryptUpdate() or
                 * EVP_DecryptUpdate() should be made with the output
                 * parameter out set to NULL.
                 */
                Ns_TclPrintfResult(interp, "could not set additional authenticated data (AAD)");
                result = TCL_ERROR;

            } else {
                int          cipherBlockSize = EVP_CIPHER_block_size(cipher), outputLength;
                Tcl_Obj     *listObj;
                Tcl_DString  outputDs, tagDs;

                Tcl_DStringInit(&outputDs);
                Tcl_DStringInit(&tagDs);

                /*
                 * Everything is set up successfully, now do the "real" encryption work.
                 *
                 * Provide the message to be encrypted, and obtain the
                 * encrypted output.  EVP_EncryptUpdate can be called
                 * multiple times if necessary.
                 */
                Tcl_DStringSetLength(&outputDs, inputLength + cipherBlockSize);
                (void)EVP_EncryptUpdate(ctx, (unsigned char *)outputDs.string, &length,
                                        (const unsigned char *)inputString, inputLength);
                outputLength = length;

                //fprintf(stderr, "allocated size %d, inputLength %d cipherBlockSize %d actual size %d\n",
                // (inputLength + cipherBlockSize), inputLength, cipherBlockSize, outputLength);
                assert((inputLength + cipherBlockSize) >= outputLength);

                (void)EVP_EncryptFinal_ex(ctx, (unsigned char  *)(outputDs.string + length), &length);
                outputLength += length;
                //fprintf(stderr, "allocated size %d, final size %d\n", (inputLength + cipherBlockSize), outputLength);
                Tcl_DStringSetLength(&outputDs, outputLength);

                /*
                 * Get the tag
                 */
                Tcl_DStringSetLength(&tagDs, 16);
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tagDs.length, tagDs.string);

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
                assert(result == TCL_OK);
                Tcl_DStringFree(&outputDs);
                Tcl_DStringFree(&tagDs);
            }

        } else {
            /*
             * Decrypt ...
             */
            assert(!encrypt);

            if (tagObj == NULL) {
                Ns_TclPrintfResult(interp, "option '-tag' has to be provided for decryption");
                result = TCL_ERROR;

            } else if ((EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
                       || (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, ivLength, NULL) != 1)
                       || (EVP_DecryptInit_ex(ctx, NULL, NULL,
                                              (const unsigned char *)keyString,
                                              (const unsigned char *)ivString) != 1)
                       ) {
                Ns_TclPrintfResult(interp, "could not initialize decryption context");
                result = TCL_ERROR;

            } else if (EVP_DecryptUpdate(ctx, NULL, &length, (const unsigned char *)aadString, aadLength) != 1) {
                /*
                 * To specify additional authenticated data (AAD), a call
                 * to EVP_CipherUpdate(), EVP_EncryptUpdate() or
                 * EVP_DecryptUpdate() should be made with the output
                 * parameter out set to NULL.
                 */
                Ns_TclPrintfResult(interp, "could not set additional authenticated data (AAD)");
                result = TCL_ERROR;

            } else {
                int          tagLength, outputLength;
                Tcl_DString  outputDs, tagDs;
                char        *tagString;

                Tcl_DStringInit(&outputDs);
                Tcl_DStringInit(&tagDs);

                /*
                 * Everything is set up successfully, now do the "real" decryption work.
                 *
                 * Provide the input to be decrypted, and obtain the plaintext output.
                 * EVP_DecryptUpdate can be called multiple times if necessary.
                 */
                Tcl_DStringSetLength(&outputDs, inputLength);
                (void)EVP_DecryptUpdate(ctx,
                                        (unsigned char *)outputDs.string, &length,
                                        (const unsigned char *)inputString, inputLength);
                outputLength = length;

                /*
                 * Set expected tag value. Works in OpenSSL 1.0.1d and later
                 */
                tagString = (char *)Ns_GetBinaryString(tagObj, NS_FALSE, &tagLength, &tagDs);

                if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tagLength, tagString) != 1) {
                    Ns_TclPrintfResult(interp, "could not set tag value");
                    result = TCL_ERROR;
                } else {

                    (void)EVP_DecryptFinal_ex(ctx, (unsigned char  *)(outputDs.string + length), &length);
                    outputLength += length;
                    //fprintf(stderr, "allocated size %d, final size %d\n", inputLength, outputLength);
                    Tcl_DStringSetLength(&outputDs, outputLength);
                    Tcl_SetObjResult(interp, EncodedObj((unsigned char *)outputDs.string,
                                                        (size_t)outputDs.length,
                                                        NULL, encoding));
                    assert(result == TCL_OK);
                }
                Tcl_DStringFree(&outputDs);
                Tcl_DStringFree(&tagDs);
            }

        }
        /* Clean up */
        EVP_CIPHER_CTX_free(ctx);
    }

    Tcl_DStringFree(&inputDs);
    Tcl_DStringFree(&aadDs);
    Tcl_DStringFree(&keyDs);
    Tcl_DStringFree(&ivDs);

    return result;
}

static int
CryptoAeadEncryptStringObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    return CryptoAeadStringObjCmd(clientData, interp, objc, objv, NS_TRUE);
}
static int
CryptoAeadDecryptStringObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    return CryptoAeadStringObjCmd(clientData, interp, objc, objv, NS_FALSE);
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoAeadEncryptObjCmd, NsTclCryptoAeadDecryptObjCmd --
 *
 *      returns encrypted/decrypted data
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
NsTclCryptoAeadEncryptObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoAeadEncryptStringObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
int
NsTclCryptoAeadDecryptObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
 *        Command to obtain random bytes from OpenSSL.
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
NsTclCryptoRandomBytesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result, nrBytes = 0;
    char              *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;
    Ns_ObjvValueRange  lengthRange = {1, INT_MAX};
    Ns_ObjvSpec lopts[] = {
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"bytes", Ns_ObjvInt, &nrBytes, &lengthRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else {
        Tcl_DString ds;
        int         rc;

        Tcl_DStringInit(&ds);
        Tcl_DStringSetLength(&ds, nrBytes);
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
NsTclCryptoEckeyObjCmd (ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
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
NsTclCryptoHmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoMdObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoAeadDecryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}
int
NsTclCryptoAeadEncryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoRandomBytesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoEckeyObjCmd (ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsCryptoScryptObjCmd (ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL 3.0 built into NaviServer");
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
