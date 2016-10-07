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

#if OPENSSL_VERSION_NUMBER < 0x010100000
# define NS_EVP_MD_CTX_new  EVP_MD_CTX_create
# define NS_EVP_MD_CTX_free EVP_MD_CTX_destroy

static HMAC_CTX *HMAC_CTX_new(void);
static void HMAC_CTX_free(HMAC_CTX *ctx) NS_GNUC_NONNULL(1);

#else
# define NS_EVP_MD_CTX_new  EVP_MD_CTX_new
# define NS_EVP_MD_CTX_free EVP_MD_CTX_free
#endif

/*
 * Static functions defined in this file.
 */

static int GetDigest(Tcl_Interp *interp, const char *digestName, const EVP_MD **mdPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

#if OPENSSL_VERSION_NUMBER > 0x010000000
static void ListMDfunc(const EVP_MD *m, const char *from, const char *to, void *arg);
#endif

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

#if OPENSSL_VERSION_NUMBER < 0x010100000
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
    CRYPTO_set_mem_functions(ns_malloc, ns_realloc, ns_free);
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    SSL_library_init();
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
                  NS_TLS_SSL **sslPtr)
{
    NS_TLS_SSL     *ssl;
    int             rc = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(sslPtr != NULL);
    
    ssl = SSL_new(ctx);
    *sslPtr = ssl;
    if (ssl == NULL) {
	Ns_TclPrintfResult(interp, "SSLCreate failed: %s", ERR_error_string(ERR_get_error(), NULL));
	return TCL_ERROR;
    }
    
    SSL_set_fd(ssl, sock);
    SSL_set_connect_state(ssl);
    
    for (;;) {
	int rc, err;

	Ns_Log(Debug, "ssl connect");
	rc  = SSL_connect(ssl);
	err = SSL_get_error(ssl, rc);

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
	rc = TCL_ERROR;
    }

    return rc;
}

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
CryptoHmacNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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

            keyString = Ns_GetBinaryString(keyObj, &keyLength);
            ctx = HMAC_CTX_new();
            HMAC_Init_ex(ctx, keyString, keyLength, md, NULL);
            Ns_TclSetAddrObj(Tcl_GetObjResult(interp), hmacCtxType, ctx);
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
CryptoHmacAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
        
        message = (const unsigned char *)Ns_GetBinaryString(messageObj, &messageLength);
        HMAC_Update(ctx, message, (size_t)messageLength);
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
CryptoHmacGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int            result = TCL_OK;
    HMAC_CTX      *ctx;
    const Tcl_Obj *ctxObj;
    Ns_ObjvSpec    args[] = {
        {"ctx",      Ns_ObjvObj, &ctxObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else if (Ns_TclGetOpaqueFromObj(ctxObj, hmacCtxType, (void **)&ctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", hmacCtxType);
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
        
        Ns_HexString( digest, digestChars, (int)mdLength, NS_FALSE);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(digestChars, (int)mdLength*2));
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
CryptoHmacFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
CryptoHmacStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int            result = TCL_OK;
    Tcl_Obj       *keyObj, *messageObj;
    const char    *digestName = "sha256";
    Ns_ObjvSpec    lopts[] = {
        {"-digest",  Ns_ObjvString, &digestName, NULL},
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
                    
            /*
             * All input parameters are valid, get key and data.
             */
            keyString = Ns_GetBinaryString(keyObj, &keyLength);
            messageString = Ns_GetBinaryString(messageObj, &messageLength);
                    
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
             * Convert the result to hex and return the hex string.
             */
            Ns_HexString( digest, digestChars, (int)mdLength, NS_FALSE);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(digestChars, (int)mdLength*2));
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
NsTclCryptoHmacObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
CryptoMdNewObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
CryptoMdAddObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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

        message = Ns_GetBinaryString(messageObj, &messageLength);
        EVP_DigestUpdate(mdctx, message, (size_t)messageLength);
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
CryptoMdGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int            result = TCL_OK;
    EVP_MD_CTX    *mdctx;
    const Tcl_Obj *ctxObj;
    Ns_ObjvSpec    args[] = {
        {"ctx", Ns_ObjvObj, &ctxObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpaqueFromObj(ctxObj, mdCtxType, (void **)&mdctx) != TCL_OK) {
        Ns_TclPrintfResult(interp, "argument is not of type \"%s\"", mdCtxType);
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

        Ns_HexString( digest, digestChars, (int)mdLength, NS_FALSE);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(digestChars, (int)mdLength*2));
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
CryptoMdFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
 *	Tcl Result Code.
 *
 * Side effects:
 *	Creating HMAC context
 *
 *----------------------------------------------------------------------
 */
static int
CryptoMdStringObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int            result = TCL_OK;
    Tcl_Obj       *messageObj;
    const char    *digestName = "sha256";
    Ns_ObjvSpec lopts[] = {
        {"-digest",  Ns_ObjvString, &digestName, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const EVP_MD  *md;

        /* 
         * Look up the Message Digest from OpenSSL
         */
        result = GetDigest(interp, digestName, &md);
        if (result != TCL_ERROR) {
            unsigned char  digest[EVP_MAX_MD_SIZE];
            char           digestChars[EVP_MAX_MD_SIZE*2 + 1];
            EVP_MD_CTX    *mdctx;
            const char    *messageString;
            int            messageLength;
            unsigned int   mdLength;

            /*
             * All input parameters are valid, get key and data.
             */
            messageString = Ns_GetBinaryString(messageObj, &messageLength);
        
            /*
             * Call the Digest computation
             */
            mdctx = NS_EVP_MD_CTX_new();
            EVP_DigestInit_ex(mdctx, md, NULL);
            EVP_DigestUpdate(mdctx, messageString, (unsigned long)messageLength);
            EVP_DigestFinal_ex(mdctx, digest, &mdLength);
            NS_EVP_MD_CTX_free(mdctx);    
                        
            /*
             * Convert the result to hex and return the hex string.
             */
            Ns_HexString( digest, digestChars, (int)mdLength, NS_FALSE);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(digestChars, (int)mdLength*2));
        }
    }
    
    return result;
}





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
NsTclCryptoMdObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"string",  CryptoMdStringObjCmd},
        {"new",     CryptoMdNewObjCmd},
        {"add",     CryptoMdAddObjCmd},
        {"get",     CryptoMdGetObjCmd},
        {"free",    CryptoMdFreeObjCmd},
        {NULL, NULL}
    };
    
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
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

int
NsTclCryptoHmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *CONST* UNUSED(objv))
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}

int
NsTclCryptoMdObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *CONST* UNUSED(objv))
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
