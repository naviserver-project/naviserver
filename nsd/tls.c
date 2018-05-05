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
 *      Support for openssl support (ssl and tls), mostly for https
 */

#include "nsd.h"

#ifdef HAVE_OPENSSL_EVP_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>

/*
 * OpenSSL < 0.9.8f does not have SSL_set_tlsext_host_name() In some
 * versions, this function is defined as a macro, on some versions as
 * a library call, which complicates detection via m4
 */
#if OPENSSL_VERSION_NUMBER > 0x00908070
# define HAVE_SSL_set_tlsext_host_name 1
# define HAVE_SSL_HMAC_CTX 1
#endif

#if defined(LIBRESSL_VERSION_NUMBER)
# if LIBRESSL_VERSION_NUMBER >= 0x2060300fL
#  define LIBRESSL_1_0_2
# endif
#endif

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_1_0_2)
# define HAVE_OPENSSL_PRE_1_1
#endif

#ifdef HAVE_SSL_HMAC_CTX
# if OPENSSL_VERSION_NUMBER < 0x010100000 || defined(LIBRESSL_1_0_2)
#  define NS_EVP_MD_CTX_new  EVP_MD_CTX_create
#  define NS_EVP_MD_CTX_free EVP_MD_CTX_destroy

static HMAC_CTX *HMAC_CTX_new(void);
static void HMAC_CTX_free(HMAC_CTX *ctx) NS_GNUC_NONNULL(1);

# else
#  define NS_EVP_MD_CTX_new  EVP_MD_CTX_new
#  define NS_EVP_MD_CTX_free EVP_MD_CTX_free
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

static int GetDigest(Tcl_Interp *interp, const char *digestName, const EVP_MD **mdPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

# if OPENSSL_VERSION_NUMBER > 0x010000000
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

/*
 * Local variables defined in this file.
 */

static const char * const mdCtxType  = "ns:mdctx";
static const char * const hmacCtxType  = "ns:hmacctx";

# if OPENSSL_VERSION_NUMBER < 0x010100000 || defined(LIBRESSL_1_0_2)
/*
 *----------------------------------------------------------------------
 *
 * HMAC_CTX_new, HMAC_CTX_free --
 *
 *	Compatibility functions for older versions of OpenSSL.  The
 *      NEW/FREE interface for HMAC_CTX is new in OpenSSL 1.1.0.
 *      Before, HMAC_CTX_init and HMAC_CTX_cleanup were used. We
 *      provide here a forward compatible version.
 *
 *----------------------------------------------------------------------
 */

/*
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
#endif

#ifdef HAVE_OPENSSL_PRE_1_1
/*
 * The function ECDSA_SIG_get0 is new in OpenSSL 1.1.0 (and not
 * available in LIBRESSL)
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
#endif


#if 0
static void hexPrint(const char *msg, unsigned char *octects, size_t octectLength)
{
    size_t i;
    fprintf(stderr, "%s octectLength %zu:", msg, octectLength);
    for (i=0; i<octectLength; i++) {
        fprintf(stderr, "%.2x ",octects[i] & 0xff);
    }
    fprintf(stderr, "\n");
}
#endif



/*
 *----------------------------------------------------------------------
 *
 * NsOpenSSLInit --
 *
 *	Library entry point for OpenSSL. This routine calls various
 *	initialization functions for OpenSSL. OpenSSL cannot be used
 *	before this function is called.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Numerous inside OpenSSL.
 *
 *----------------------------------------------------------------------
 */
void NsInitOpenSSL(void)
{
#ifdef HAVE_OPENSSL_EVP_H
# if OPENSSL_VERSION_NUMBER < 0x10100000L
    CRYPTO_set_mem_functions(ns_malloc, ns_realloc, ns_free);
# endif
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
# if OPENSSL_VERSION_NUMBER < 0x010100000 || defined(LIBRESSL_1_0_2)
    SSL_library_init();
# else
    OPENSSL_init_ssl(0, NULL);
# endif
    Ns_Log(Notice, "%s initialized", SSLeay_version(SSLEAY_VERSION));
#endif
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
        Ns_TclPrintfResult(interp, "ctx init failed: %s", ERR_error_string(ERR_get_error(), NULL));
        return TCL_ERROR;
    }

    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_load_verify_locations(ctx, caFile, caPath);
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);

    if (cert != NULL) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            Ns_TclPrintfResult(interp, "certificate load error: %s", ERR_error_string(ERR_get_error(), NULL));
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
#if HAVE_SSL_set_tlsext_host_name
            Ns_Log(Debug, "tls: setting SNI hostname '%s'", sni_hostname);
            if (SSL_set_tlsext_host_name(ssl, sni_hostname) != 1) {
                Ns_Log(Warning, "tls: setting SNI hostname '%s' failed, value ignored", sni_hostname);
            }
#else
            Ns_Log(Warning, "tls: SNI hostname '%s' is not supported by version of OpenSSL", sni_hostname);
#endif
        }
        SSL_set_fd(ssl, sock);
        SSL_set_connect_state(ssl);

        for (;;) {
            int sslRc, err;

            Ns_Log(Debug, "ssl connect");
            sslRc = SSL_connect(ssl);
            err   = SSL_get_error(ssl, sslRc);

            if ((err == SSL_ERROR_WANT_WRITE) || (err == SSL_ERROR_WANT_READ)) {
                Ns_Time timeout = { 0, 10000 }; /* 10ms */
                (void) Ns_SockTimedWait(sock,
                                        ((unsigned int)NS_SOCK_WRITE|(unsigned int)NS_SOCK_READ),
                                        &timeout);
                continue;
            }
            break;
        }

        if (!SSL_is_init_finished(ssl)) {
            Ns_TclPrintfResult(interp, "ssl connect failed: %s", ERR_error_string(ERR_get_error(), NULL));
            result = TCL_ERROR;
        }
    }

    return result;
}

#ifdef HAVE_SSL_HMAC_CTX
#if OPENSSL_VERSION_NUMBER > 0x010000000
/*
 *----------------------------------------------------------------------
 *
 * ListMDfunc --
 *
 *      Helper function for iterator EVP_MD_do_all_sorted
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Appending to passed Tcl list
 *
 *----------------------------------------------------------------------
 */

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
#endif

/*
 *----------------------------------------------------------------------
 *
 * GetDigest --
 *
 *      Helper function to lookup digest from a string.
 *
 * Results:
 *	Tcl result code, value in third argument.
 *
 * Side effects:
 *	Interp result Obj is updated.
 *
 *----------------------------------------------------------------------
 */
static int
GetDigest(Tcl_Interp *interp, const char *digestName, const EVP_MD **mdPtr)
{
    int result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(digestName != NULL);
    NS_NONNULL_ASSERT(mdPtr != NULL);

    *mdPtr = EVP_get_digestbyname(digestName);
    if (*mdPtr == NULL) {
#if OPENSSL_VERSION_NUMBER > 0x010000000
        /*
         * EVP_MD_do_all_sorted was added in OpenSSL 1.0.0
         */
        Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

        Tcl_IncrRefCount(listObj);
        EVP_MD_do_all_sorted(ListMDfunc, listObj);
        Ns_TclPrintfResult(interp, "Unknown value for digest \"%s\", valid: %s",
                           digestName, Tcl_GetString(listObj));
        Tcl_DecrRefCount(listObj);
#else
        Ns_TclPrintfResult(interp, "Unknown message digest \"%s\"", digestName);
#endif
        result = TCL_ERROR;
    } else {
        result = TCL_OK;
    }
    return result;
}


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
        Ns_TclPrintfResult(interp, "Unknown value for output encoding \"%s\", valid: hex, base64url, base64, binary",
                           name);
        result = TCL_ERROR;
    }
    return result;
}

static void
SetEncodedResultObj(Tcl_Interp *interp, unsigned char *octects, size_t octectLength,
                    char *outputBuffer, Ns_ResultEncoding encoding) {
    char *origOutputBuffer = outputBuffer;

    if (outputBuffer == NULL && encoding != RESULT_ENCODING_BINARY) {
        /*
         * It is a safe assumption to double the size, since the hex
         * encoding needs to most space.
         */
        outputBuffer = ns_malloc(octectLength * 2u + 1u);
    }

    switch (encoding) {
    case RESULT_ENCODING_BINARY:
        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((const unsigned char *)outputBuffer, (int)octectLength));
        break;

    case RESULT_ENCODING_BASE64URL:
        //hexPrint("result", octects, octectLength);
        (void)Ns_HtuuEncode2(octects, octectLength, outputBuffer, 1);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(outputBuffer, (int)strlen(outputBuffer)));
        break;

    case RESULT_ENCODING_BASE64:
        (void)Ns_HtuuEncode2(octects, octectLength, outputBuffer, 0);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(outputBuffer, (int)strlen(outputBuffer)));
        break;

    case RESULT_ENCODING_HEX: /* fall through */
    default:
        Ns_HexString(octects, outputBuffer, (int)octectLength, NS_FALSE);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(outputBuffer, (int)octectLength*2));
        break;
    }

    if (outputBuffer != origOutputBuffer) {
        ns_free(outputBuffer);
    }

}


/*
 *----------------------------------------------------------------------
 *
 * CryptoHmacNewObjCmd -- Subcommand of NsTclCryptoHmacObjCmd
 *
 *        Incremental command to initialize a HMAC context. This
 *        command is typically followed by a sequence of "add"
 *        subcommands until the content is read with the "get"
 *        subcommand an then freed.
 *
 * Results:
 *	Tcl Result Code.
 *
 * Side effects:
 *	Creating HMAC context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoHmacNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int            result = TCL_OK;
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
            keyString = Ns_GetBinaryString(keyObj, &keyLength, &keyDs);
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	Updating HMAC context
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
        message = (const unsigned char *)Ns_GetBinaryString(messageObj, &messageLength, &messageDs);
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	None.
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
        SetEncodedResultObj(interp, digest, mdLength, digestChars, encoding);
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	Freeing memory
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CryptoHmacStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK;
    Tcl_Obj           *keyObj, *messageObj;
    const char        *digestName = "sha256";
    char              *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec    lopts[] = {
        {"-digest",  Ns_ObjvString, &digestName, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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
            keyString = Ns_GetBinaryString(keyObj, &keyLength, &keyDs);
            messageString = Ns_GetBinaryString(messageObj, &messageLength, &messageDs);
            //hexPrint("hmac message", messageString, messageLength);

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
            SetEncodedResultObj(interp, digest, mdLength, digestChars, encoding);

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
 *	NS_OK
 *
 * Side effects:
 *	Tcl result is set to a string value.
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
 *        subcommand an then freed.
 *
 * Results:
 *	Tcl Result Code.
 *
 * Side effects:
 *	Creating MD context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int            result = TCL_OK;
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	Updating MD context.
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
        message = Ns_GetBinaryString(messageObj, &messageLength, &messageDs);
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	None.
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
        SetEncodedResultObj(interp, digest, mdLength, digestChars, encoding);
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	Freeing memory
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	Creating HMAC context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK;
    Tcl_Obj           *messageObj;
    char              *digestName = (char *)"sha256", *keyFile = NULL, *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-digest",   Ns_ObjvString, &digestName, NULL},
        {"-sign",     Ns_ObjvString, &keyFile, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
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
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR && keyFile != NULL) {
            PW_CB_DATA  cb_data;
            BIO        *bio = NULL;

            cb_data.password = "";
#if 0
            sigkey  = load_key(keyFile, OPT_FMT_ANY, 0,
                               NULL /*pass phrase*/,
                               NULL /*engine, maybe hardware*/,
                               "key file");
            key = bio_open_default(file, 'r', format);

            ::ns_crypto::md string -digest sha256 -sign /usr/local/src/naviserver/private.pem "hello\n"
#endif

            bio = BIO_new_file(keyFile, "r");
            if (bio == NULL) {
                Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", keyFile);
                result = TCL_ERROR;
            } else {

                pkey = PEM_read_bio_PrivateKey(bio, NULL,
                                               (pem_password_cb *)password_callback,
                                               &cb_data);
                BIO_free(bio);

                if (pkey == NULL) {
                    fprintf(stderr, "got no pkey\n");
                    result = TCL_ERROR;
                }
            }
        }
        if (result != TCL_ERROR) {
            unsigned char  digest[EVP_MAX_MD_SIZE];
            char           digestChars[EVP_MAX_MD_SIZE*2 + 1], *outputBuffer = digestChars;
            EVP_MD_CTX    *mdctx;
            EVP_PKEY_CTX  *pctx;
            const char    *messageString;
            int            messageLength;
            unsigned int   mdLength;
            Tcl_DString    messageDs;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&messageDs);
            messageString = Ns_GetBinaryString(messageObj, &messageLength, &messageDs);

            /*
             * Call the Digest or Signature computation
             */
            mdctx = NS_EVP_MD_CTX_new();
            if (pkey != NULL) {
                int r = EVP_DigestSignInit(mdctx, &pctx, md, NULL /*engine*/, pkey);

                if (r == 0) {
                    fprintf(stderr, "could not initialize signature context\n");
                    pctx = NULL;
                    mdLength = 0u;
                } else {
                    (void)EVP_DigestSignUpdate(mdctx, messageString, messageLength);
                    (void)EVP_DigestSignFinal(mdctx, digest, (size_t*)&mdLength);
                    fprintf(stderr, "final signature length %u\n",mdLength);
                    outputBuffer = ns_malloc(mdLength * 2u + 1u);
                }
                EVP_PKEY_free(pkey);

            } else {
                EVP_DigestInit_ex(mdctx, md, NULL);
                EVP_DigestUpdate(mdctx, messageString, (unsigned long)messageLength);
                EVP_DigestFinal_ex(mdctx, digest, &mdLength);
            }

            NS_EVP_MD_CTX_free(mdctx);

            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            SetEncodedResultObj(interp, digest, mdLength, outputBuffer, encoding);
            if (outputBuffer != digestChars) {
                ns_free(outputBuffer);
            }
            Tcl_DStringFree(&messageDs);
        }
    }

    return result;
}

/*
 */
static int
CryptoVapidSignObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK;
    Tcl_Obj           *messageObj;
    char              *digestName = (char *)"sha256", *pemFile = NULL, *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-digest",   Ns_ObjvString, &digestName, NULL},
        {"-pem",      Ns_ObjvString, &pemFile, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
      set ::pemFile /usr/local/ns/modules/vapid/prime256v1_key.pem
      ::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile  "hello"
      ::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile -encoding hex "hello"
      ::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile -encoding base64url "hello"

      proc vapidToken {string} {
          return $string.[::ns_crypto::md vapidsign -digest sha256 -pem $::pemFile -encoding base64url $string]
      }

      vapidToken "hello"

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
        EVP_PKEY     *pkey = NULL;
        EC_KEY       *eckey = NULL;

        /*
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {
            PW_CB_DATA  cb_data;
            BIO        *bio = NULL;

            cb_data.password = "";

            bio = BIO_new_file(pemFile, "r");
            if (bio == NULL) {
                Ns_TclPrintfResult(interp, "could not open pem file '%s' for reading", pemFile);
                result = TCL_ERROR;
            } else {

                pkey = PEM_read_bio_PrivateKey(bio, NULL,
                                               (pem_password_cb *)password_callback,
                                               &cb_data);
                BIO_free(bio);
                if (pkey == NULL) {
                    Ns_TclPrintfResult(interp, "pem file contains no private key");
                    result = TCL_ERROR;

                } else {
                    eckey = EVP_PKEY_get1_EC_KEY(pkey);
                    if (eckey == NULL) {
                        Ns_TclPrintfResult(interp, "no valid EC key in specified pem file");
                        result = TCL_ERROR;
                    }
                }
            }
        }
        if (result != TCL_ERROR) {
            unsigned char  digest[EVP_MAX_MD_SIZE];
            EVP_MD_CTX    *mdctx;
            const char    *messageString;
            int            messageLength;
            unsigned int   sigLen = 0u;
            unsigned int   mdLength, rLen, sLen;
            Tcl_DString    messageDs;
            ECDSA_SIG     *sig = NULL;
            const BIGNUM  *r, *s;
            uint8_t       *rawSig = NULL;

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&messageDs);
            messageString = Ns_GetBinaryString(messageObj, &messageLength, &messageDs);

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
            if (rawSig != NULL) {
                BN_bn2bin(r, rawSig);
                //hexPrint("r", rawSig, rLen);
                BN_bn2bin(s, &rawSig[rLen]);
                //hexPrint("s", &rawSig[rLen], sLen);
            }

            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            SetEncodedResultObj(interp, rawSig, sigLen, NULL, encoding);

            /*
             * Clean up.
             */
            EVP_PKEY_free(pkey);
            NS_EVP_MD_CTX_free(mdctx);
            ns_free(rawSig);
            Tcl_DStringFree(&messageDs);
        }
    }

    return result;
}

#ifndef HAVE_OPENSSL_PRE_1_1
#include <openssl/kdf.h>

/*
 * HMAC-based Extract-and-Expand Key Derivation Function (HKDF)
 * RFC 5869 https://tools.ietf.org/html/rfc5869
 */
static int
CryptoMdHkdfObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                result = TCL_OK, outLength;
    Tcl_Obj           *saltObj = NULL, *secretObj = NULL, *infoObj = NULL;
    char              *digestName = (char *)"sha256", *outputEncodingString = NULL;
    Ns_ResultEncoding  encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-digest",   Ns_ObjvString, &digestName, NULL},
        {"-salt",     Ns_ObjvObj,    &saltObj, NULL},
        {"-secret",   Ns_ObjvObj,    &secretObj, NULL},
        {"-info",     Ns_ObjvObj,    &infoObj, NULL},
        {"-encoding", Ns_ObjvString, &outputEncodingString, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"length", Ns_ObjvInt, &outLength, NULL},
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

    } else if (outLength < 1) {
        Ns_TclPrintfResult(interp, "the specified length must be a positive value");
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

            /*
             * All input parameters are valid, get key and data.
             */
            Tcl_DStringInit(&saltDs);
            Tcl_DStringInit(&secretDs);
            Tcl_DStringInit(&infoDs);
            keyString = ns_malloc((size_t)outLength);

            saltString   = Ns_GetBinaryString(saltObj,   &saltLength,   &saltDs);
            secretString = Ns_GetBinaryString(secretObj, &secretLength, &secretDs);
            infoString   = Ns_GetBinaryString(infoObj,   &infoLength,   &infoDs);

            //hexPrint("salt  ", saltString, saltLength);
            //hexPrint("secret", secretString, secretLength);
            //hexPrint("info  ", infoString, infoLength);

            if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, saltString, saltLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set salt");
                result = TCL_ERROR;
            } else if (EVP_PKEY_CTX_set1_hkdf_key(pctx, secretString, secretLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set secret");
                result = TCL_ERROR;
            } else if (EVP_PKEY_CTX_add1_hkdf_info(pctx, infoString, infoLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not set info");
                result = TCL_ERROR;
            } else if (EVP_PKEY_derive(pctx, keyString, (size_t *)&outLength) <= 0) {
                Ns_TclPrintfResult(interp, "could not obtain dereived key");
                result = TCL_ERROR;
            }

            if (result == TCL_OK) {
                /*
                 * Convert the result to the output format and set the interp
                 * result.
                 */
                SetEncodedResultObj(interp, keyString, (size_t)outLength, NULL, encoding);
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
#endif


/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoMdObjCmd --
 *
 *      Returns a Hash-based message authentication code of the provided message
 *
 * Results:
 *	NS_OK
 *
 * Side effects:
 *	Tcl result is set to a string value.
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
        {"vapidsign", CryptoVapidSignObjCmd},
#ifndef HAVE_OPENSSL_PRE_1_1
        {"hkdf",      CryptoMdHkdfObjCmd},
#endif
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * GetCipher --
 *
 *      Helper function to lookup cipher from a string.
 *
 * Results:
 *	Tcl result code, value in third argument.
 *
 * Side effects:
 *	Interp result Obj is updated.
 *
 *----------------------------------------------------------------------
 */
static int
GetCipher(Tcl_Interp *interp, const char *cipherName, const EVP_CIPHER **cipherPtr)
{
    int result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cipherName != NULL);
    NS_NONNULL_ASSERT(cipherPtr != NULL);

    *cipherPtr = EVP_get_cipherbyname(cipherName);
    if (*cipherPtr == NULL) {
        Ns_TclPrintfResult(interp, "Unknown cipher \"%s\"", cipherName);
        result = TCL_ERROR;
    } else {
        result = TCL_OK;
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * CryptoEncStringObjCmd -- Subcommand of NsTclCryptoEncObjCmd
 *
 *        Single command to encrypt/decrypt string data.  The command
 *        returns the encrypted string in the specified encoding.
 *        Currently, only encryption is supported.
 *
 * Results:
 *	Tcl Result Code.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static int
CryptoEncStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                  result = TCL_OK, encryptInt = 1;
    Tcl_Obj             *messageObj, *ivObj = NULL, *keyObj = NULL, *aadObj = NULL;
    char                *cipherName = (char *)"aes-128-gcm";
    const EVP_CIPHER    *cipher;
    const unsigned char *keyString = NULL;
    Tcl_DString          ivDs, cipherDs, keyDs, aadDs;
    char                *outputEncodingString = NULL;
    Ns_ResultEncoding    encoding = RESULT_ENCODING_HEX;

    Ns_ObjvSpec lopts[] = {
        {"-cipher",   Ns_ObjvString,  &cipherName, NULL},
        {"-iv",       Ns_ObjvObj,     &ivObj,      NULL},
        {"-aad",      Ns_ObjvObj,     &aadObj,     NULL},
        {"-key",      Ns_ObjvObj,     &keyObj,     NULL},
        {"-enccrypt", Ns_ObjvBool,    &encryptInt,  NULL},
        {"-encoding", Ns_ObjvString,  &outputEncodingString, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    Tcl_DStringInit(&ivDs);

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (keyObj == NULL) {
        Ns_TclPrintfResult(interp, "no key in specified");
        result = TCL_ERROR;

    } else if (outputEncodingString != NULL
               && GetResultEncoding(interp, outputEncodingString, &encoding) != TCL_OK) {
        /*
         * Function cares about error message
         */
        result = TCL_ERROR;

    } else if ((result = GetCipher(interp, cipherName, &cipher)) == TCL_OK) {
        EVP_CIPHER_CTX      *ctx = EVP_CIPHER_CTX_new();
        const unsigned char *messageString, *ivString, *aadString;
        int                  messageLength, ivLength, aadLength, keyLength, length;

        /*
              ::ns_crypto::enc string -cipher aes-128-gcm -iv 123456789 -key secret "hello world"
        */
        Tcl_DStringInit(&cipherDs);
        Tcl_DStringInit(&keyDs);
        Tcl_DStringInit(&aadDs);
        keyString = (const unsigned char*)Ns_GetBinaryString(keyObj, &keyLength, &keyDs);

        /*
         * Get optional additional authenticated data (AAD)
         */
        if (aadObj != NULL) {
            aadString = (const unsigned char*)Ns_GetBinaryString(aadObj, &aadLength, &aadDs);
        } else {
            aadLength = 0;
            aadString = (const unsigned char *)"";
        }

        /*
         * Get sometimes optional initialization vector (IV)
         */
        if (ivObj != NULL) {
            ivString = (const unsigned char*)Ns_GetBinaryString(ivObj, &ivLength, &ivDs);
        } else {
            ivString = NULL;
            ivLength = 0;
        }

        if (ivLength > EVP_MAX_IV_LENGTH
            || (ivLength == 0 && EVP_CIPHER_iv_length(cipher) > 0)
            ) {
            Ns_TclPrintfResult(interp, "initialization vector is invalid (default length for %s: %d bytes)",
                               cipherName, EVP_CIPHER_iv_length(cipher), NULL);
            result = TCL_ERROR;

        } else if (ctx == NULL) {
            Ns_TclPrintfResult(interp, "could not create encryption context", NULL);
            result = TCL_ERROR;

        } else if ((EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
                   || (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, ivLength, NULL) != 1)
                   || (EVP_EncryptInit_ex(ctx, NULL, NULL, keyString, (const unsigned char *)ivString) != 1)
                   ) {
            Ns_TclPrintfResult(interp, "could not initialize encryption context", NULL);
            result = TCL_ERROR;

        } else if (EVP_EncryptUpdate(ctx, NULL, &length, (const unsigned char *)aadString, aadLength) != 1) {
            /*
             * To specify additional authenticated data (AAD), a call to
             * EVP_CipherUpdate(), EVP_EncryptUpdate() or
             * EVP_DecryptUpdate() should be made with the output
             * parameter out set to NULL.
             */
            Ns_TclPrintfResult(interp, "could not set additional authenticated data (AAD)", NULL);
            result = TCL_ERROR;

        } else {
            Tcl_DString  messageDs;
            int          cipherBlockSize = EVP_CIPHER_block_size(cipher), cipherLength = 0;

            /*
             * Everything is set successfully, now do the "real" encryption work.
             */
            Tcl_DStringInit(&messageDs);

            //fprintf(stderr, "allocatedEVP_EncryptUpdate after aadString %d\n", length);
            messageString = (const unsigned char *)Ns_GetBinaryString(messageObj, &messageLength, &messageDs);

            /*
             * Provide the message to be encrypted, and obtain the
             * encrypted output.  EVP_EncryptUpdate can be called
             * multiple times if necessary.
             */
            Tcl_DStringSetLength(&cipherDs, messageLength + cipherBlockSize);
            (void)EVP_EncryptUpdate(ctx, (unsigned char  *)cipherDs.string, &length, messageString, messageLength);
            cipherLength = length;

            //fprintf(stderr, "allocated size %d, messageLength %d cipherBlockSize %d actual size %d\n",
            // (messageLength + cipherBlockSize), messageLength, cipherBlockSize, cipherLength);
            assert((messageLength + cipherBlockSize) >= cipherLength);

            (void)EVP_EncryptFinal_ex(ctx, (unsigned char  *)(cipherDs.string + length), &length);
            cipherLength += length;
            //fprintf(stderr, "allocated size %d, final size %d\n", (messageLength + cipherBlockSize), cipherLength);
            Tcl_DStringSetLength(&cipherDs, cipherLength);

            /* Get the tag */
            //EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag)

            /* Clean up */
            Tcl_DStringFree(&messageDs);
            EVP_CIPHER_CTX_free(ctx);
        }

        if (result == TCL_OK) {
            /*
             * Convert the result to the output format and set the interp
             * result.
             */
            SetEncodedResultObj(interp, (unsigned char *)cipherDs.string, (size_t)cipherDs.length,
                                NULL, encoding);
        }
    }
    if (keyString != NULL) {
        Tcl_DStringInit(&aadDs);
        Tcl_DStringInit(&keyDs);
        Tcl_DStringInit(&cipherDs);
    }
    Tcl_DStringFree(&ivDs);

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptoEncObjCmd --
 *
 *      returs encrypted/decrypted data
 *
 * Results:
 *	NS_OK
 *
 * Side effects:
 *	Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCryptoEncObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoEncStringObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

#endif

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
                       const char *cert, const char *caFile, const char *caPath, bool verify,
                       const char *ciphers,
                       NS_TLS_SSL_CTX **ctxPtr)
{
    NS_TLS_SSL_CTX *ctx;
    int rc;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctxPtr != NULL);

    ctx = SSL_CTX_new(SSLv23_server_method());
    *ctxPtr = ctx;
    if (ctx == NULL) {
        Ns_TclPrintfResult(interp, "ctx init failed: %s", ERR_error_string(ERR_get_error(), NULL));
        return TCL_ERROR;
    }

    rc = SSL_CTX_set_cipher_list(ctx, ciphers);
    if (rc == 0) {
        Ns_TclPrintfResult(interp, "ctx cipher list failed: %s", ERR_error_string(ERR_get_error(), NULL));
        goto fail;
    }

    if (cert == NULL && caFile == NULL) {
        Ns_TclPrintfResult(interp, "At least one of certificate or cafile must be specified!");
        goto fail;
    }

    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_load_verify_locations(ctx, caFile, caPath);
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);

    if (cert != NULL) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            Ns_TclPrintfResult(interp, "certificate load error: %s", ERR_error_string(ERR_get_error(), NULL));
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
 * Ns_TLS_SSLAccept --
 *
 *   Initialize a socket as ssl socket and wait until the socket is usable (is
 *   accepted, handshake performed)
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
        Ns_TclPrintfResult(interp, "SSLAccept failed: %s", ERR_error_string(ERR_get_error(), NULL));
        Ns_Log(Debug, "SSLAccept failed: %s", ERR_error_string(ERR_get_error(), NULL));
        result = TCL_ERROR;

    } else {

        SSL_set_fd(ssl, sock);
        SSL_set_accept_state(ssl);

        for (;;) {
            int rc, sslerr;

            rc = SSL_do_handshake(ssl);
            sslerr = SSL_get_error(ssl, rc);

            if (sslerr == SSL_ERROR_WANT_WRITE || sslerr == SSL_ERROR_WANT_READ) {
                Ns_Time timeout = { 0, 10000 }; /* 10ms */

                (void) Ns_SockTimedWait(sock, ((unsigned int)NS_SOCK_WRITE|(unsigned int)NS_SOCK_READ), &timeout);
                continue;
            }
            break;
        }

        if (!SSL_is_init_finished(ssl)) {
            Ns_TclPrintfResult(interp, "ssl accept failed: %s", ERR_error_string(ERR_get_error(), NULL));
            Ns_Log(Debug, "SSLAccept failed: %s", ERR_error_string(ERR_get_error(), NULL));

            SSL_free(ssl);
            *sslPtr = NULL;
            result = TCL_ERROR;
        }
    }
    return result;
}


#else

void NsInitOpenSSL(void)
{
    Ns_Log(Notice, "No support for OpenSSL compiled in");
}

/*
 * Dummy stub functions, for the case, when NaviServer is built without
 * openssl support, e.g. when built for the option --without-openssl.
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
                       const char *UNUSED(cert), const char *UNUSED(caFile), const char *UNUSED(caPath), bool UNUSED(verify),
                       const char *UNUSED(ciphers),
                       NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    Ns_TclPrintfResult(interp, "CtxServerCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *UNUSED(ctx))
{
    /* dummy stub */
}
#endif

#ifndef HAVE_SSL_HMAC_CTX
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
NsTclCryptoEncObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
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
