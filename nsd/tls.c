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
 * Static functions defined in this file.
 */

static const char *GetString(Tcl_Obj *obj, int *lengthPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#if OPENSSL_VERSION_NUMBER > 0x010000000
static void ListMDfunc(const EVP_MD *m, const char *from, const char *to, void *arg);
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
                 const char *cert, const char *caFile, const char *caPath, int verify,
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
	    return TCL_ERROR;
	}

	if (SSL_CTX_use_PrivateKey_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
	    Ns_TclPrintfResult(interp, "private key load error: %s", ERR_error_string(ERR_get_error(), NULL));
	    return TCL_ERROR;
	}
    }
    
    return TCL_OK;
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
 *   Initialize a socket as ssl socket and wait until the socket is usable (is
 *   connected, handshake performed)
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
    
    while (1) {
	int rc, err;

	Ns_Log(Debug, "ssl connect");
	rc  = SSL_connect(ssl);
	err = SSL_get_error(ssl, rc);

	if ((err == SSL_ERROR_WANT_WRITE) || (err == SSL_ERROR_WANT_READ)) {
	    Ns_Time timeout = { 0, 10000 }; /* 10ms */
	    Ns_SockTimedWait(sock, (unsigned int)(NS_SOCK_WRITE|NS_SOCK_READ), &timeout);
	    continue;
	}
	break;
    }

    if (!SSL_is_init_finished(ssl)) {
	Ns_TclPrintfResult(interp, "ssl connect failed: %s", ERR_error_string(ERR_get_error(), NULL));
	return TCL_ERROR;
    }

    return TCL_OK;
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
ListMDfunc(const EVP_MD *m, const char *from, const char *to, void *arg) {
    Tcl_Obj *listPtr = (Tcl_Obj *)arg;
    
    if (m != NULL && from != NULL) {
        const char *mdName = EVP_MD_name(m);
        
        /* fprintf(stderr, "from %s to %to name <%s> type (nid) %d\n",from,to,mdName, EVP_MD_type(m)); */
        /*
         * Apprarently, the list contains upper and lower case variants. Avoid
         * duplication.
         */
        if (*from >= 'a' && *from <= 'z') {
            (void)Tcl_ListObjAppendElement(NULL, listPtr, Tcl_NewStringObj(mdName, -1));
        }
    }
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * GetString --
 *
 *      Helper function to return the content either binary or as text
 *
 * Results:
 *	Content of the Tcl_Obj.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static const char *
GetString(Tcl_Obj *obj, int *lengthPtr)
{
    const char *result;

    NS_NONNULL_ASSERT(obj != NULL);
    NS_NONNULL_ASSERT(lengthPtr != NULL);
    
    if (NsTclObjIsByteArray(obj) == NS_TRUE) {
        result = (char *)Tcl_GetByteArrayFromObj(obj, lengthPtr);
    } else {
        result = Tcl_GetStringFromObj(obj, lengthPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHmacObjCmd --
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
NsTclHmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    unsigned char  digest[EVP_MAX_MD_SIZE];
    char           digestChars[EVP_MAX_MD_SIZE*2 + 1];
    Tcl_Obj       *keyObj, *messageObj;
    const char    *key, *message, *digestName = "sha256";
    int            keyLength, messageLength;
    unsigned int   mdLength;
    const EVP_MD  *md;
    Ns_ObjvSpec lopts[] = {
        {"-digest",  Ns_ObjvString, &digestName, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"key",     Ns_ObjvObj, &keyObj, NULL},
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    /* 
     * Look up the Message digest from OpenSSL
     */
    md = EVP_get_digestbyname(digestName);
    if (md == NULL) {
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
        return TCL_ERROR;
    }

    /*
     * All input parameters are valid, get key and data.
     */
    key = GetString(keyObj, &keyLength);
    message = GetString(messageObj, &messageLength);

    /*
     * Call the HMAC computation
     */
#if OPENSSL_VERSION_NUMBER < 0x010100000
    /*
     * The interface has changed in OpenSSL 1.1.0
     */
    {
        HMAC_CTX ctx;
        HMAC_CTX_init(&ctx);
        HMAC(md,
             (const void *)key, keyLength,
             (const void *)message, messageLength,
             digest, &mdLength);
        HMAC_CTX_cleanup(&ctx);
    }
#else
    {
        HMAC_CTX *ctx = HMAC_CTX_new();
        HMAC(md,
             (const void *)key, keyLength,
             (const void *)message, messageLength,
             digest, &mdLength);
        HMAC_CTX_free(ctx);
    }
#endif    
    
    /*
     * Convert the result to hex and return the hex string.
     */
    Ns_HexString( digest, digestChars, mdLength, NS_FALSE);
    Tcl_AppendResult(interp, digestChars, NULL);
    
    return NS_OK;
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
Ns_TLS_CtxClientCreate(Tcl_Interp *interp,
                       const char *UNUSED(cert), const char *UNUSED(caFile), const char *UNUSED(caPath), int UNUSED(verify),
                       NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    Ns_TclPrintfResult(interp, "CtxCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *UNUSED(ctx))
{
    /* dummy stub */
}

int
NsTclHmacObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_TclPrintfResult(interp, "Command requires support for OpenSSL built into NaviServer");
    return TCL_ERROR;
}
#endif
 
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
