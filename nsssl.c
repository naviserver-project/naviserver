/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * Copyright (C) 2001-2012 Vlad Seryakov
 * Copyright (C) 2012 Gustaf Neumann
 * All rights reserved.
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
 * nsssl.c -- HTTP over SSL driver
 *
 *
 * Authors
 *
 *     Vlad Seryakov vlad@crystalballinc.com
 *     Gustaf Neumann neumann@wu-wien.ac.at
 */

#include "ns.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define NSSSL_VERSION  "0.9"

NS_EXTERN bool NsTclObjIsByteArray(const Tcl_Obj *objPtr);

typedef struct {
    SSL_CTX     *ctx;
    Ns_Mutex     lock;
    int          verify;
    int          deferaccept;  /* Enable the TCP_DEFER_ACCEPT optimization. */
    DH          *dhKey512;     /* Fallback Diffie Hellman keys of length 512 */
    DH          *dhKey1024;    /* Fallback Diffie Hellman keys of length 1024 */
} SSLDriver;

typedef struct {
    SSL         *ssl;
    int          verified;
} SSLContext;

typedef struct {
    Ns_HttpTask http;
    SSL_CTX     *ctx;
    SSL         *ssl;
} Https;

/*
 * Local functions defined in this file
 */

static Ns_DriverListenProc Listen;
static Ns_DriverAcceptProc Accept;
static Ns_DriverRecvProc Recv;
static Ns_DriverSendProc Send;
static Ns_DriverKeepProc Keep;
static Ns_DriverCloseProc Close;

static int SSLInterpInit(Tcl_Interp *interp, const void *arg);
static int SSLObjCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);
static int SSLPassword(char *buf, int num, int rwflag, void *userdata);
static void SSLLock(int mode, int n, const char *file, int line);
static unsigned long SSLThreadId(void);
static int HttpsConnect(Tcl_Interp *interp, char *method, char *url, Ns_Set *hdrPtr,
			Tcl_Obj *bodyPtr, char *cert, char *caFile, char *caPath, int verify, bool keep_host_header,
			Https **httpsPtrPtr)
        NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(7);
static void HttpsClose(Https *httpsPtr);
static void HttpsCancel(Https *httpsPtr);
static void HttpsAbort(Https *httpsPtr);
static Https *HttpsGet(Tcl_Interp *interp, char *id);
static Ns_TaskProc HttpsProc;

static DH *SSL_dhCB(SSL *ssl, int isExport, int keyLength);

/*
 * Static variables defined in this file.
 */

static Ns_Mutex *driver_locks;
static Ns_TaskQueue *session_queue;
static Tcl_HashTable session_table;
static Ns_Mutex session_lock;

NS_EXPORT int Ns_ModuleVersion = 1;

static void 
SSL_infoCB(const SSL *ssl, int where, int ret) {
    if ((where & SSL_CB_HANDSHAKE_DONE)) {
        ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
    }
}

/*
 * Include pre-generated DH parameters 
 */
#include "dhparams.h"

/*
 * Callback used for ephemeral DH keys
 */
static DH *
SSL_dhCB(SSL *ssl, int isExport, int keyLength) {
    SSLDriver *drvPtr;
    DH *key;

    Ns_Log(Debug, "SSL_dhCB: isExport %d keyLength %d", isExport, keyLength);
    drvPtr = (SSLDriver *) SSL_get_app_data(ssl);

    key = 0;
    switch (keyLength) {
    case 512:
        key = drvPtr->dhKey512;
        break;

    case 1024:
    default:
        key = drvPtr->dhKey1024;
    }
    Ns_Log(Debug, "SSL_dhCB: returns %p\n", (void *)key);
    return key;
}

NS_EXPORT int
Ns_ModuleInit(char *server, char *module)
{
    Ns_DString ds;
    int num;
    const char *path, *value;
    SSLDriver *drvPtr;
    Ns_DriverInitData init = {0};

    Ns_DStringInit(&ds);

    path = Ns_ConfigGetPath(server, module, (char *)0);

    drvPtr = ns_calloc(1, sizeof(SSLDriver));
    drvPtr->deferaccept = Ns_ConfigBool(path, "deferaccept", NS_FALSE);
    drvPtr->verify = Ns_ConfigBool(path, "verify", 0);

    init.version = NS_DRIVER_VERSION_3;
    init.name = "nsssl";
    init.listenProc = Listen;
    init.acceptProc = Accept;
    init.recvProc = Recv;
    init.sendProc = Send;
    init.sendFileProc = NULL;
    init.keepProc = Keep;
    init.requestProc = NULL;
    init.closeProc = Close;
    init.opts = NS_DRIVER_SSL|NS_DRIVER_ASYNC;
    init.arg = drvPtr;
    init.path = path;

    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "nsssl: driver init failed.");
        ns_free(drvPtr);
        return NS_ERROR;
    }

    num = CRYPTO_num_locks();
    driver_locks = ns_calloc(num, sizeof(*driver_locks));
    {   int n;
        for (n = 0; n < num; n++) {
            Ns_DStringPrintf(&ds, "nsssl:%d", n);
            Ns_MutexSetName(driver_locks + n, ds.string);
            Ns_DStringTrunc(&ds, 0);
        }
    }
    CRYPTO_set_locking_callback(SSLLock);
    CRYPTO_set_id_callback(SSLThreadId);
    CRYPTO_set_mem_functions(ns_malloc, ns_realloc, ns_free);

    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    SSL_library_init();

    drvPtr->ctx = SSL_CTX_new(SSLv23_server_method());
    if (drvPtr->ctx == NULL) {
        Ns_Log(Error, "nsssl: init error [%s]", strerror(errno));
        return NS_ERROR;
    }
    SSL_CTX_set_app_data(drvPtr->ctx, drvPtr);

    /*
     * Get default keys and save it in the driver data for fast reuse.
     */
    drvPtr->dhKey512 = get_dh512();
    drvPtr->dhKey1024 = get_dh1024();

    /* 
     * Load certificate and private key 
     */
    value = Ns_ConfigGetValue(path, "certificate");
    if (value == NULL) {
        Ns_Log(Error, "nsssl: certificate parameter should be specified under %s", path);
        return NS_ERROR;
    }
    if (SSL_CTX_use_certificate_chain_file(drvPtr->ctx, value) != 1) {
        Ns_Log(Error, "nsssl: certificate load error [%s]", ERR_error_string(ERR_get_error(), NULL));
        return NS_ERROR;
    }
    if (SSL_CTX_use_PrivateKey_file(drvPtr->ctx, value, SSL_FILETYPE_PEM) != 1) {
        Ns_Log(Error, "nsssl: private key load error [%s]", ERR_error_string(ERR_get_error(), NULL));
        return NS_ERROR;
    }

    /* 
     * Get DH parameters from .pem file
     */
    {
        BIO *bio = BIO_new_file(value, "r");
        DH  *dh  = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
        BIO_free(bio);

        if (dh != NULL) {
            if (SSL_CTX_set_tmp_dh(drvPtr->ctx, dh) < 0) {
                Ns_Log(Error, "nsssl: Couldn't set DH parameters");
                return NS_ERROR;
            }
            DH_free(dh);
        }
    }

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
	SSL_CTX_set_options(drvPtr->ctx, SSL_OP_SINGLE_ECDH_USE);
	if (SSL_CTX_set_tmp_ecdh(drvPtr->ctx, ecdh) != 1) {
	    Ns_Log(Error, "nsssl: Couldn't set ecdh parameters");
	    return NS_ERROR;
	}
	EC_KEY_free (ecdh);
    }

    /* 
     * Https cache support
     */
    Ns_DStringPrintf(&ds, "nsssl:%d", getpid());
    SSL_CTX_set_session_id_context(drvPtr->ctx, (void *) ds.string, ds.length);
    SSL_CTX_set_session_cache_mode(drvPtr->ctx, SSL_SESS_CACHE_SERVER);

    /*
     * Parse SSL protocols
     */
    {
        long n = SSL_OP_ALL;
        value = Ns_ConfigGetValue(path, "protocols");
        if (value != NULL) {
            if (strstr(value, "!SSLv2") != NULL) {
                n |= SSL_OP_NO_SSLv2;
                Ns_Log(Notice, "nsssl: disabling SSLv2");
            }
            if (strstr(value, "!SSLv3") != NULL) {
                n |= SSL_OP_NO_SSLv3;
                Ns_Log(Notice, "nsssl: disabling SSLv3");
            }
            if (strstr(value, "!TLSv1") != NULL) {
                n |= SSL_OP_NO_TLSv1;
                Ns_Log(Notice, "nsssl: disabling TLSv1");
            }
        }
        SSL_CTX_set_options(drvPtr->ctx, n);
    }

    /*
     * Set info callback to prevent client-initiated renegotiation
     * (after the handshake).
     */
    SSL_CTX_set_info_callback(drvPtr->ctx, SSL_infoCB);

    /*
     * Parse SSL ciphers
     */
    value = Ns_ConfigGetValue(path, "ciphers");
    if (value != NULL && SSL_CTX_set_cipher_list(drvPtr->ctx, value) == 0) {
        Ns_Log(Error, "nsssl: error loading ciphers: %s", value);
    }

    SSL_CTX_set_default_passwd_cb(drvPtr->ctx, SSLPassword);
    SSL_CTX_set_mode(drvPtr->ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_SINGLE_DH_USE);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_SSLEAY_080_CLIENT_DH_BUG);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_TLS_D5_BUG);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_TLS_BLOCK_PADDING_BUG);

    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
    /*
     * Prefer server ciphers to secure against BEAST attack.
     */
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_NO_SSLv2);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_SINGLE_DH_USE);
    /*
     * Disable compression to avoid CRIME attack.
     */
#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_NO_COMPRESSION);
#endif
    if (drvPtr->verify) {
        SSL_CTX_set_verify(drvPtr->ctx, SSL_VERIFY_PEER, NULL);
    }

    SSL_CTX_set_tmp_dh_callback(drvPtr->ctx, SSL_dhCB);

    /*
     * Seed the OpenSSL Pseudo-Random Number Generator.
     */
    Ns_DStringSetLength(&ds, 1024);
    for (num = 0; !RAND_status() && num < 3; num++) {
        int n;
        Ns_Log(Notice, "nsssl: Seeding OpenSSL's PRNG");
        for (n = 0; n < 1024; n++) {
            ds.string[n] = Ns_DRand();
        }
        RAND_seed(ds.string, 1024);
    }
    if (!RAND_status()) {
        Ns_Log(Warning, "nsssl: PRNG fails to have enough entropy");
    }

    Tcl_InitHashTable(&session_table, TCL_STRING_KEYS);

    Ns_TclRegisterTrace(server, SSLInterpInit, drvPtr, NS_TCL_TRACE_CREATE);
    Ns_DStringFree(&ds);
    Ns_Log(Notice, "nsssl: version %s loaded, based on %s", NSSSL_VERSION, SSLeay_version(SSLEAY_VERSION));
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Listen --
 *
 *      Open a listening socket in non-blocking mode.
 *
 * Results:
 *      The open socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
Listen(Ns_Driver *driver, CONST char *address, int port, int backlog)
{
    NS_SOCKET sock;

    sock = Ns_SockListenEx((char*)address, port, backlog);
    if (sock != NS_INVALID_SOCKET) {
	SSLDriver *cfg = driver->arg;

        (void) Ns_SockSetNonBlocking(sock);
	if (cfg->deferaccept) {
	  Ns_SockSetDeferAccept(sock, driver->recvwait);
	}
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Accept --
 *
 *      Accept a new socket in non-blocking mode.
 *
 * Results:
 *      NS_DRIVER_ACCEPT_DATA  - socket accepted, data present
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
 
static NS_DRIVER_ACCEPT_STATUS
Accept(Ns_Sock *sock, NS_SOCKET listensock, struct sockaddr *sockaddrPtr, socklen_t *socklenPtr)
{
    SSLDriver *drvPtr = sock->driver->arg;
    SSLContext *sslPtr = sock->arg;

    sock->sock = Ns_SockAccept(listensock, sockaddrPtr, socklenPtr);
    if (sock->sock != NS_INVALID_SOCKET) {
#ifdef __APPLE__
      /* 
       * Darwin's poll returns per default writable in situations,
       * where nothing can be written.  Setting the socket option for
       * the send low watermark to 1 fixes this problem.
       */
        int value = 1;
	setsockopt(sock->sock, SOL_SOCKET,SO_SNDLOWAT, &value, sizeof(value));
#endif
        Ns_SockSetNonBlocking(sock->sock);

        if (sslPtr == NULL) {
            sslPtr = ns_calloc(1, sizeof(SSLContext));
            sslPtr->ssl = SSL_new(drvPtr->ctx);
            if (sslPtr->ssl == NULL) {
                char ipString[NS_IPADDR_SIZE];
                Ns_Log(Error, "%d: SSL session init error for %s: [%s]", 
		       sock->sock, 
		       ns_inet_ntop((struct sockaddr *)&(sock->sa), ipString, sizeof(ipString)),
		       strerror(errno));
                ns_free(sslPtr);
                return NS_DRIVER_ACCEPT_ERROR;
            }
            sock->arg = sslPtr;
            SSL_set_fd(sslPtr->ssl, sock->sock);
	    SSL_set_mode(sslPtr->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
            SSL_set_accept_state(sslPtr->ssl);
            SSL_set_app_data(sslPtr->ssl, drvPtr);
            SSL_set_tmp_dh_callback(sslPtr->ssl, SSL_dhCB);
        }
        return NS_DRIVER_ACCEPT_DATA;
    }
    return NS_DRIVER_ACCEPT_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Recv --
 *
 *      Receive data into given buffers.
 *
 * Results:
 *      Total number of bytes received or -1 on error or timeout.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Recv(Ns_Sock *sock, struct iovec *bufs, int nbufs, Ns_Time *timeoutPtr, unsigned int flags)
{
    SSLDriver *drvPtr = sock->driver->arg;
    SSLContext *sslPtr = sock->arg;
    int got = 0;
    char *p = (char *)bufs->iov_base;

    /*
     * Verify client certificate, driver may require valid cert
     */

    if (drvPtr->verify && sslPtr->verified == 0) {
	X509 *peer;
        if ((peer = SSL_get_peer_certificate(sslPtr->ssl))) {
             X509_free(peer);
             if (SSL_get_verify_result(sslPtr->ssl) != X509_V_OK) {
                 char ipString[NS_IPADDR_SIZE];
                 Ns_Log(Error, "nsssl: client certificate not valid by %s",
                        ns_inet_ntop((struct sockaddr *)&(sock->sa), ipString, sizeof(ipString)));
                 return NS_ERROR;
             }
        } else {
            char ipString[NS_IPADDR_SIZE];
            Ns_Log(Error, "nsssl: no client certificate provided by %s",
                   ns_inet_ntop((struct sockaddr *)&(sock->sa), ipString, sizeof(ipString)));
            return NS_ERROR;
        }
        sslPtr->verified = 1;
    }

    while (1) {
	int err, n;

        ERR_clear_error();
        n = SSL_read(sslPtr->ssl, p + got, bufs->iov_len - got);
        err = SSL_get_error(sslPtr->ssl, n);

        switch (err) {
        case SSL_ERROR_NONE: 
            if (n < 0) { 
		fprintf(stderr, "### SSL_read should not happen\n"); 
		return n;
	    }
            /*fprintf(stderr, "### SSL_read %d pending %d\n", n, SSL_pending(sslPtr->ssl));*/
	    got += n;
            if (n == 1 && got < bufs->iov_len) {
                /*fprintf(stderr, "### SSL retry after read of %d bytes\n", n);*/
                continue;
            }
            /*Ns_Log(Notice, "### SSL_read %d got <%s>", got, p);*/
	    return got;
            
        case SSL_ERROR_WANT_READ: 
            /*fprintf(stderr, "### SSL_read WANT_READ returns %d\n", got);*/
            return got;
            
        default:
            /*fprintf(stderr, "### SSL_read error\n");*/
            SSL_set_shutdown(sslPtr->ssl, SSL_RECEIVED_SHUTDOWN);
            return -1;
        }
    }
    
    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Send data from given buffers.
 *
 * Results:
 *      Total number of bytes sent or -1 on error or timeout.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Send(Ns_Sock *sock, const struct iovec *bufs, int nbufs, 
     const Ns_Time *timeoutPtr, unsigned int flags)
{
    SSLContext *sslPtr = sock->arg;
    int         rc, size;
    bool        decork;

    size = 0;
    decork = Ns_SockCork(sock, NS_TRUE);
    while (nbufs > 0) {
	if (bufs->iov_len > 0) {
	    ERR_clear_error();
	    rc = SSL_write(sslPtr->ssl, bufs->iov_base, bufs->iov_len);

	    if (rc < 0) {
		if (SSL_get_error(sslPtr->ssl, rc) == SSL_ERROR_WANT_WRITE) {
		    Ns_Time timeout = { sock->driver->sendwait, 0 };
		    if (Ns_SockTimedWait(sock->sock, NS_SOCK_WRITE, &timeout) == NS_OK) {
			continue;
		    }
		}
		if (decork == NS_TRUE) {
                    Ns_SockCork(sock, NS_FALSE);
                }
		SSL_set_shutdown(sslPtr->ssl, SSL_RECEIVED_SHUTDOWN);
		return -1;
	    }
	    size += rc;
	    if (rc < bufs->iov_len) {
		Ns_Log(Debug, "SSL: partial write, wanted %ld wrote %d", bufs->iov_len, rc);
		break;
	    }
	}
	nbufs--;
	bufs++;
    }

    if (decork == NS_TRUE) {
        Ns_SockCork(sock, NS_FALSE);
    }
    return size;
}


/*
 *----------------------------------------------------------------------
 *
 * Keep --
 *
 *      No keepalives
 *
 * Results:
 *      0, always.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
Keep(Ns_Sock *sock)
{
    SSLContext *sslPtr = sock->arg;

    if (SSL_get_shutdown(sslPtr->ssl) == 0) {
        BIO *bio = SSL_get_wbio(sslPtr->ssl);
        if (bio != NULL && BIO_flush(bio) == 1) {
            return NS_TRUE;
        }
    }
    return NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Close --
 *
 *      Close the connection socket.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Does not close UDP socket
 *
 *----------------------------------------------------------------------
 */

static void
Close(Ns_Sock *sock)
{
    SSLContext *sslPtr = sock->arg;

    if (sslPtr != NULL) {
	int i;
        for (i = 0; i < 4 && !SSL_shutdown(sslPtr->ssl); i++);
        SSL_free(sslPtr->ssl);
        ns_free(sslPtr);
    }
    if (sock->sock > -1) {
        ns_sockclose(sock->sock);
        sock->sock = -1;
    }
    sock->arg = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * SSLInterpInit --
 *
 *      Add ns_ssl commands to interp.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SSLInterpInit(Tcl_Interp *interp, const void *arg)
{
    Tcl_CreateObjCommand(interp, "ns_ssl", SSLObjCmd, (ClientData)arg, NULL);
    return NS_OK;
}

static int
SSLPassword(char *buf, int num, int rwflag, void *userdata)
{
    fprintf(stdout, "Enter SSL password:");
    (void) fgets(buf, num, stdin);
    return(strlen(buf));
}

static void
SSLLock(int mode, int n, const char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        Ns_MutexLock(driver_locks + n);
    } else {
        Ns_MutexUnlock(driver_locks + n);
    }
}

static unsigned long
SSLThreadId(void)
{
    return (unsigned long) Ns_ThreadId();
}


/*
 *----------------------------------------------------------------------
 *
 * SSLObjCmd --
 *
 *  Implements the new ns_ssl to handle HTTP requests.
 *
 * Results:
 *  Standard Tcl result.
 *
 * Side effects:
 *  May queue an HTTP request.
 *
 *----------------------------------------------------------------------
 */

static int
SSLObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Https *httpsPtr = NULL;
    Ns_HttpTask *httpPtr;
    Ns_Set *hdrPtr = NULL;
    Ns_Time *timeoutPtr = NULL;
    int opt, run = 0;

    static CONST char *opts[] = {
       "cancel", "cleanup", "run", "queue", "wait", "list",
       NULL
    };
    enum {
        HCancelIdx, HCleanupIdx, HRunIdx, HQueueIdx, HWaitIdx, HListIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?args ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case HRunIdx:
    run = 1;

    case HQueueIdx: {
        int verify = 0, flag, i;
        char *cert = NULL;
        char buf[32], *url = NULL, *method = "GET", *caFile = NULL, *caPath = NULL;
        Tcl_Obj *bodyPtr = NULL;
	bool keep_host_header = NS_FALSE;

        Ns_ObjvSpec opts[] = {
            {"-timeout",  Ns_ObjvTime,    &timeoutPtr,  NULL},
            {"-headers",  Ns_ObjvSet,     &hdrPtr,      NULL},
            {"-method",   Ns_ObjvString,  &method,      NULL},
            {"-cert",     Ns_ObjvString,  &cert,        NULL},
            {"-cafile",   Ns_ObjvString,  &caFile,      NULL},
            {"-capath",   Ns_ObjvString,  &caPath,      NULL},
            {"-body",     Ns_ObjvObj,     &bodyPtr,     NULL},
            {"-verify",   Ns_ObjvBool,    &verify,      NULL},
	    {"-keep_host_header", Ns_ObjvBool, &keep_host_header, INT2PTR(NS_TRUE)},
            {NULL, NULL,  NULL, NULL}
        };
        Ns_ObjvSpec args[] = {
            {"url",       Ns_ObjvString,  &url,         NULL},
            {NULL, NULL, NULL, NULL}
        };
        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }
	if (HttpsConnect(interp, method, url, hdrPtr, bodyPtr, 
			 cert, caFile, caPath, verify, keep_host_header,
			 &httpsPtr) != TCL_OK) {
	    return TCL_ERROR;
	}

	httpPtr = &httpsPtr->http;
        Ns_GetTime(&httpPtr->stime);
        httpPtr->timeout = httpPtr->stime;

        if (timeoutPtr != NULL) {
            Ns_IncrTime(&httpPtr->timeout, timeoutPtr->sec, timeoutPtr->usec);
        } else {
            Ns_IncrTime(&httpPtr->timeout, 2, 0);
        }
        httpPtr->task = Ns_TaskCreate(httpPtr->sock, HttpsProc, httpsPtr);
        if (run) {
            Ns_TaskRun(httpPtr->task);
        } else {
            if (session_queue == NULL) {
                Ns_MasterLock();
                if (session_queue == NULL) {
                    session_queue = Ns_CreateTaskQueue("tclssl");
                }
                Ns_MasterUnlock();
            }
            if (Ns_TaskEnqueue(httpPtr->task, session_queue) != NS_OK) {
                HttpsClose(httpsPtr);
                Tcl_AppendResult(interp, "could not queue ssl task", NULL);
                return TCL_ERROR;
            }
        }
        Ns_MutexLock(&session_lock);
        i = session_table.numEntries;
        do {
            sprintf(buf, "ssl%d", i++);
            hPtr = Tcl_CreateHashEntry(&session_table, buf, &flag);
        } while (!flag);
        Tcl_SetHashValue(hPtr, httpsPtr);
        Ns_MutexUnlock(&session_lock);

        Tcl_SetResult(interp, buf, TCL_VOLATILE);
        break;
    }

    case HWaitIdx: {
        Tcl_Obj *elapsedVarPtr = NULL;
        Tcl_Obj *resultVarPtr = NULL;
        Tcl_Obj *statusVarPtr = NULL;
        Tcl_Obj *fileVarPtr = NULL;
        Tcl_Obj *valPtr;
        char *id = NULL;
        Ns_Time diff;
	int spoolLimit = -1, decompress = 0;

        Ns_ObjvSpec opts[] = {
            {"-timeout",    Ns_ObjvTime, &timeoutPtr,    NULL},
            {"-headers",    Ns_ObjvSet,  &hdrPtr,        NULL},
            {"-elapsed",    Ns_ObjvObj,  &elapsedVarPtr, NULL},
            {"-result",     Ns_ObjvObj,  &resultVarPtr,  NULL},
            {"-status",     Ns_ObjvObj,  &statusVarPtr,  NULL},
	    {"-file",       Ns_ObjvObj,  &fileVarPtr,    NULL},
	    {"-spoolsize",  Ns_ObjvInt,  &spoolLimit,    NULL},
	    {"-decompress", Ns_ObjvBool, &decompress,    INT2PTR(NS_TRUE)},
            {NULL, NULL,  NULL, NULL}
        };
        Ns_ObjvSpec args[] = {
            {"id",       Ns_ObjvString, &id, NULL},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }

        if (!(httpsPtr = HttpsGet(interp, id))) {
            return TCL_ERROR;
        }
	httpPtr = &httpsPtr->http;

	if (decompress) {
	  httpPtr->flags |= NS_HTTP_FLAG_DECOMPRESS;
	}

	if (hdrPtr == NULL) {
	  /*
	   * If no output headers are provided, we create our
	   * own. The ns_set is needed for checking the content
	   * length of the reply.
	   */
	  hdrPtr = Ns_SetCreate("outputHeaders");
	}
	httpPtr->spoolLimit = spoolLimit;
	httpPtr->replyHeaders = hdrPtr;

	Ns_HttpCheckSpool(httpPtr);

        if (Ns_TaskWait(httpPtr->task, timeoutPtr) != NS_OK) {
            HttpsCancel(httpsPtr);
            Tcl_AppendResult(interp, "timeout waiting for task", NULL);
            return TCL_ERROR;
        }
	
        if (elapsedVarPtr != NULL) {
            Ns_DiffTime(&httpPtr->etime, &httpPtr->stime, &diff);
            valPtr = Tcl_NewObj();
            Ns_TclSetTimeObj(valPtr, &diff);
            if (!Ns_SetNamedVar(interp, elapsedVarPtr, valPtr)) {
		HttpsClose(httpsPtr);
		return TCL_ERROR;
            }
        }

        if (httpPtr->error) {
            Tcl_AppendResult(interp, "ns_ssl failed: ", httpPtr->error, NULL);
            HttpsClose(httpsPtr);
            return TCL_ERROR;
        }

	if (httpPtr->replyHeaderSize == 0) {
	    Ns_HttpCheckHeader(httpPtr);
	}
	Ns_HttpCheckSpool(httpPtr);

	Ns_Log(Ns_LogTaskDebug, "SSL request finished %d <%s>", httpPtr->status, httpPtr->replyHeaders->name);
	if (httpPtr->status == 0) {
	    Ns_Log(Ns_LogTaskDebug, "======= SSL response <%s>", httpPtr->ds.string);
	}

        if (statusVarPtr != NULL && !Ns_SetNamedVar(interp, statusVarPtr, Tcl_NewIntObj(httpPtr->status))) {
            HttpsClose(httpsPtr);
            return TCL_ERROR;
        }
	
	if (httpPtr->spoolFd > 0)  {
	    close(httpPtr->spoolFd);
	    valPtr = Tcl_NewObj();
	} else {
            bool binary = NS_TRUE;

            if (hdrPtr != NULL) {
                const char *contentEncoding = Ns_SetIGet(hdrPtr, "Content-Encoding");
            
                /*
                 * Does the contentEncoding allow text transfers? Not, if the
                 * content is compressed.
                 */

                if (contentEncoding == NULL || strncmp(contentEncoding, "gzip", 4u) != 0) {
                    const char *contentType = Ns_SetIGet(hdrPtr, "Content-Type");
                
                    if (contentType != NULL) {
                        /*
                         * Determine binary via contentType
                         */
                        binary = Ns_IsBinaryMimeType(contentType);
                    }
                }
            }

            if (binary == NS_TRUE)  {
                valPtr = Tcl_NewByteArrayObj((unsigned char*)httpPtr->ds.string + httpPtr->replyHeaderSize, 
                                             (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
            } else {
                valPtr = Tcl_NewStringObj(httpPtr->ds.string + httpPtr->replyHeaderSize, 
                                          (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
            }
        }

	if (fileVarPtr && httpPtr->spoolFd > 0 
	    && !Ns_SetNamedVar(interp, fileVarPtr, Tcl_NewStringObj(httpPtr->spoolFileName, -1))) {
	    HttpsClose(httpsPtr);
	    return TCL_ERROR;	
	}

        if (resultVarPtr == NULL) {
            Tcl_SetObjResult(interp, valPtr);
        } else {
            if (!Ns_SetNamedVar(interp, resultVarPtr, valPtr)) {
                HttpsClose(httpsPtr);
                return TCL_ERROR;
            }
            Tcl_SetBooleanObj(Tcl_GetObjResult(interp), 1);
        }
        HttpsClose(httpsPtr);
        break;
    }

    case HCancelIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "id");
            return TCL_ERROR;
        }
        if (!(httpsPtr = HttpsGet(interp, Tcl_GetString(objv[2])))) {
            return TCL_ERROR;
        }
        HttpsAbort(httpsPtr);
        break;

    case HCleanupIdx:
        Ns_MutexLock(&session_lock);
        hPtr = Tcl_FirstHashEntry(&session_table, &search);
        while (hPtr != NULL) {
            httpsPtr = Tcl_GetHashValue(hPtr);
            HttpsAbort(httpsPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&session_table);
        Tcl_InitHashTable(&session_table, TCL_STRING_KEYS);
        Ns_MutexUnlock(&session_lock);
        break;

    case HListIdx:
        Ns_MutexLock(&session_lock);
        hPtr = Tcl_FirstHashEntry(&session_table, &search);
        while (hPtr != NULL) {
            httpsPtr = Tcl_GetHashValue(hPtr);
	    httpPtr = &httpsPtr->http;
            Tcl_AppendResult(interp, Tcl_GetHashKey(&session_table, hPtr), " ",
                             httpPtr->url, " ",
                             Ns_TaskCompleted(httpPtr->task) ? "done" : "running",
                             " ", NULL);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&session_lock);
        break;
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpsGet --
 *
 *  Locate and remove the Https struct for a given id.
 *
 * Results:
 *  pointer on success, NULL otherwise.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */

static Https *
HttpsGet(Tcl_Interp *interp, char *id)
{
    Https *httpsPtr = NULL;
    Tcl_HashEntry *hPtr;

    Ns_MutexLock(&session_lock);
    hPtr = Tcl_FindHashEntry(&session_table, id);
    if (hPtr != NULL) {
        httpsPtr = Tcl_GetHashValue(hPtr);
        Tcl_DeleteHashEntry(hPtr);
    } else {
        Tcl_AppendResult(interp, "no such request: ", id, NULL);
    }
    Ns_MutexUnlock(&session_lock);
    return httpsPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * HttpsConnect --
 *
 *        Open a connection to the given URL host and construct
 *        an Http structure to fetch the file.
 *
 * Results:
 *        Tcl result code.
 *
 * Side effects:
 *        Updates httpsPtrPtr with newly allocated Https struct
 *        on success.
 *
 *----------------------------------------------------------------------
 */

int
HttpsConnect(Tcl_Interp *interp, char *method, char *url, Ns_Set *hdrPtr, Tcl_Obj *bodyPtr, 
	     char *cert, char *caFile, char *caPath, int verify, bool keep_host_header,
	     Https **httpsPtrPtr)
{
    NS_SOCKET    sock;
    Ns_HttpTask *httpPtr = NULL;
    Https       *httpsPtr = NULL;
    int          portNr, uaFlag = -1;
    char        *url2, *host, *file, *portString;
    const char  *contentType = NULL;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(httpPtrPtr != NULL);

    /*
     * Parse and split url
     */
    if (strncmp(url, "https://", 8u) != 0 || url[8] == '\0') {
	Tcl_AppendResult(interp, "invalid url: ", url, NULL);
	return TCL_ERROR;
    }

    /* 
     * If host_keep_header set then Host header must be present.
     */

    if (keep_host_header == NS_TRUE) {
        if ( hdrPtr == NULL || Ns_SetIFind(hdrPtr, "Host") == -1 ) {
	    Tcl_AppendResult(interp, "keep_host_header specified but no Host header given", NULL);
	    return TCL_ERROR;
        }
    }

    /*
     * Make a non-const copy of url, where we can replace the item separating
     * characters with '\0' characters.
     */
    url2 = ns_strdup(url);

    host = url2 + 8;
    file = strchr(host, '/');
    if (file != NULL) {
	*file = '\0';
    }

    Ns_HttpParseHost(host, &host, &portString);

    if (portString != NULL) {
        *portString = '\0';
	portNr = (int) strtol(portString + 1, NULL, 10);
    } else {
        portNr = 443;
    }

    /*
     * Connect to the host and allocate session struct
     */

    sock = Ns_SockAsyncConnect(host, portNr);
    if (sock == NS_INVALID_SOCKET) {
	Tcl_AppendResult(interp, "connect to ", url, " failed: ", ns_sockstrerror(ns_sockerrno), NULL);
        ns_free(url2);
	return TCL_ERROR;
    }

    if (bodyPtr != NULL) {
        if (hdrPtr != NULL) {
            contentType = Ns_SetIGet(hdrPtr, "Content-Type");
        }
        if (contentType == NULL) {
            Tcl_AppendResult(interp, "header field Content-Type is required when body is provided", NULL);
            ns_free(url2);
            return TCL_ERROR;
        }
    }

    httpsPtr = ns_calloc(1, sizeof(Https));
    httpPtr  = &httpsPtr->http;

    httpPtr->sock            = sock;
    httpPtr->spoolLimit      = -1;
    httpPtr->url             = url2;

    Ns_MutexInit(&httpPtr->lock);
    /*Ns_MutexSetName(&httpPtr->lock, name, buffer);*/
    Tcl_DStringInit(&httpPtr->ds);

    /*Ns_Log(Ns_LogTaskDebug, "url <%s> port %d sock %d host <%s> file <%s>", httpPtr->url, portNr, sock, host, file);*/

    /*
     * Now initialize OpenSSL context
     */
    
    httpsPtr->ctx = SSL_CTX_new(SSLv23_client_method());
    if (httpsPtr->ctx == NULL) {
	Tcl_AppendResult(interp, "ctx init failed: ", ERR_error_string(ERR_get_error(), NULL), NULL);
	HttpsClose(httpsPtr);
	return TCL_ERROR;
    }

    SSL_CTX_set_default_verify_paths(httpsPtr->ctx);
    SSL_CTX_load_verify_locations (httpsPtr->ctx, caFile, caPath);
    SSL_CTX_set_verify(httpsPtr->ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(httpsPtr->ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(httpsPtr->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    
    if (cert != NULL) {
	if (SSL_CTX_use_certificate_chain_file(httpsPtr->ctx, cert) != 1) {
	    Tcl_AppendResult(interp, "certificate load error: ", ERR_error_string(ERR_get_error(), NULL), NULL);
	    HttpsClose(httpsPtr);
	    return TCL_ERROR;
	}

	if (SSL_CTX_use_PrivateKey_file(httpsPtr->ctx, cert, SSL_FILETYPE_PEM) != 1) {
	    Tcl_AppendResult(interp, "private key load error: ", ERR_error_string(ERR_get_error(), NULL), NULL);
	    HttpsClose(httpsPtr);
	    return TCL_ERROR;
	}
    }
    
    httpsPtr->ssl = SSL_new(httpsPtr->ctx);
    if (httpsPtr->ssl == NULL) {
	Tcl_AppendResult(interp, "ssl init failed: ", ERR_error_string(ERR_get_error(), NULL), NULL);
	HttpsClose(httpsPtr);
	return TCL_ERROR;
    }
    
    SSL_set_fd(httpsPtr->ssl, sock);
    SSL_set_connect_state(httpsPtr->ssl);
    
    while (1) {
	int rc, err;

	Ns_Log(Debug, "ssl connect");
	rc = SSL_connect(httpsPtr->ssl);
	err = SSL_get_error(httpsPtr->ssl, rc);

	if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
	    Ns_Time timeout = { 0, 10000 }; /* 10ms */
	    Ns_SockTimedWait(sock, NS_SOCK_WRITE|NS_SOCK_READ, &timeout);
	    continue;
	}
	break;
    }

    if (!SSL_is_init_finished(httpsPtr->ssl)) {
	Tcl_AppendResult(interp, "ssl connect failed: ", ERR_error_string(ERR_get_error(), NULL), NULL);
	HttpsClose(httpsPtr);
	return TCL_ERROR;
    }
    
    Ns_DStringPrintf(&httpPtr->ds, "%s /%s HTTP/1.0\r\n", method, (file != NULL) ? file + 1 : "");

    /*
     * Submit provided headers
     */
    
    if (hdrPtr != NULL) {
	int i;

	/*
	 * Remove the header fields, we are providing
	 */
        if (keep_host_header == NS_FALSE) {
	    Ns_SetIDeleteKey(hdrPtr, "Host");
        }
	Ns_SetIDeleteKey(hdrPtr, "Connection");
	Ns_SetIDeleteKey(hdrPtr, "Content-Length");

	for (i = 0; i < Ns_SetSize(hdrPtr); i++) {
	    char *key = Ns_SetKey(hdrPtr, i);
	    if (uaFlag) {
		uaFlag = strcasecmp(key, "User-Agent");
	    }
	    Ns_DStringPrintf(&httpPtr->ds, "%s: %s\r\n", key, Ns_SetValue(hdrPtr, i));
	}
    }

    /*
     * No keep-alive even in case of HTTP 1.1
     */
    Ns_DStringAppend(&httpPtr->ds, "Connection: close\r\n");
    
    /*
     * User-Agent header was not supplied, add our own header
     */
    if (uaFlag) {
	Ns_DStringPrintf(&httpPtr->ds, "User-Agent: %s/%s\r\n",
			 Ns_InfoServerName(),
			 Ns_InfoServerVersion());
    }
    
    if (keep_host_header == NS_FALSE) {
        if (portString == NULL) {
	    Ns_DStringPrintf(&httpPtr->ds, "Host: %s\r\n", host);
        } else {
	    Ns_DStringPrintf(&httpPtr->ds, "Host: %s:%d\r\n", host, portNr);
        }
    }

    if (bodyPtr != NULL) {
        int length = 0;
	const char *bodyString;
        bool binary = NsTclObjIsByteArray(bodyPtr);

        if (contentType != NULL && binary == NS_FALSE) {
            /*const Tcl_Encoding enc = Ns_GetTypeEncoding(contentType);*/
            binary = Ns_IsBinaryMimeType(contentType);
        }

	if (binary == NS_TRUE) {
	    bodyString = (void *)Tcl_GetByteArrayFromObj(bodyPtr, &length);
        } else {
            bodyString = Tcl_GetStringFromObj(bodyPtr, &length);
        }
	Ns_DStringPrintf(&httpPtr->ds, "Content-Length: %d\r\n\r\n", length);
        Tcl_DStringAppend(&httpPtr->ds, bodyString, length);
        
    } else {
        Tcl_DStringAppend(&httpPtr->ds, "\r\n", 2);
    }
        
    httpPtr->next = httpPtr->ds.string;
    httpPtr->len = httpPtr->ds.length;

    /*
     *  Restore the url2 string. This modifies the string appearance of host
     *  as well.
     */
    if (file != NULL) {
	*file = '/';
    }
    if (portString != NULL) {
	*portString = ':';
    }
    
    Ns_Log(Ns_LogTaskDebug, "full request <%s>", httpPtr->ds.string);
    
    *httpsPtrPtr = httpsPtr;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HttpsClose --
 *
 *        Finish Http Task and cleanup memory
 *
 * Results:
 *        None
 *
 * Side effects:
 *        Free up memory
 *
 *----------------------------------------------------------------------
 */
static void
HttpsClose(Https *httpsPtr)
{
    Ns_HttpTask *httpPtr = &httpsPtr->http;

    if (httpPtr->task != NULL) {Ns_TaskFree(httpPtr->task);}
    if (httpsPtr->ssl != NULL) {
        SSL_shutdown(httpsPtr->ssl);
        SSL_free(httpsPtr->ssl);
    }
    if (httpsPtr->ctx != NULL)  {SSL_CTX_free(httpsPtr->ctx);}
    if (httpPtr->sock > 0)      {ns_sockclose(httpPtr->sock);}
    if (httpPtr->spoolFileName) {ns_free(httpPtr->spoolFileName);}
    if (httpPtr->spoolFd > 0)   {close(httpPtr->spoolFd);}
    if (httpPtr->compress)      {
	Ns_InflateEnd(httpPtr->compress);
	ns_free(httpPtr->compress);
    }
    Ns_MutexDestroy(&httpPtr->lock);
    Tcl_DStringFree(&httpPtr->ds);
    ns_free((char *)httpPtr->url);
    ns_free(httpsPtr);
}


static void
HttpsCancel(Https *httpsPtr)
{
    Ns_HttpTask *httpPtr = &httpsPtr->http;

    Ns_TaskCancel(httpPtr->task);
    Ns_TaskWait(httpPtr->task, NULL);
}


static void
HttpsAbort(Https *httpsPtr)
{
    HttpsCancel(httpsPtr);
    HttpsClose(httpsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpsProc --
 *
 *        Task callback for ns_http connections.
 *
 * Results:
 *        None.
 *
 * Side effects:
 *        Will call Ns_TaskCallback and Ns_TaskDone to manage state
 *        of task.
 *
 *----------------------------------------------------------------------
 */

static void
HttpsProc(Ns_Task *task, NS_SOCKET sock, void *arg, Ns_SockState why)
{
    Https       *httpsPtr = arg;
    Ns_HttpTask *httpPtr  = &httpsPtr->http;
    char buf[16384];
    int n, err, got;

    switch (why) {
    case NS_SOCK_INIT:
	Ns_TaskCallback(task, NS_SOCK_WRITE, &httpPtr->timeout);
	return;

    case NS_SOCK_WRITE:

	while (1) {
	    n = SSL_write(httpsPtr->ssl, httpPtr->next, httpPtr->len);
	    err = SSL_get_error(httpsPtr->ssl, n);
	    if (err == SSL_ERROR_WANT_WRITE) {
		Ns_Time timeout = { 0, 10000 }; /* 10ms */
		Ns_SockTimedWait(httpPtr->sock, NS_SOCK_WRITE, &timeout);
		continue;
	    }
	    break;
	}

        if (n < 0) {
            httpPtr->error = "send failed";
        } else {
            httpPtr->next += n;
            httpPtr->len -= n;
            if (httpPtr->len == 0) {
		SSL_set_shutdown(httpsPtr->ssl, SSL_SENT_SHUTDOWN);
                /*shutdown(sock, 1);*/
		/*Ns_Log(Ns_LogTaskDebug, "SSL WRITE done, switch to READ");*/
                Tcl_DStringTrunc(&httpPtr->ds, 0);
                Ns_TaskCallback(task, NS_SOCK_READ, &httpPtr->timeout);
            }
            return;
        }
        break;

    case NS_SOCK_READ:
	got = 0;
        while (1) {
	    n = SSL_read(httpsPtr->ssl, buf+got, sizeof(buf)-got);
	    err = SSL_get_error(httpsPtr->ssl, n);
	    /*fprintf(stderr, "### SSL_read n %d got %d err %d\n", n, got, err); */
	    switch (err) {
	    case SSL_ERROR_NONE: 
		if (n < 0) { 
		    fprintf(stderr, "### SSL_read should not happen\n"); 
		    break;
		}
		got += n;
		break;

	    case SSL_ERROR_WANT_READ: 
		/*fprintf(stderr, "### WANT read, n %d\n", (int)n); */
		got += n;
		continue;
	    }
	    break;
        }
	n = got;

	/*Ns_Log(Ns_LogTaskDebug, "Task READ got %d bytes err %d", (int)n, err);*/
	
        if (likely(n > 0)) {
	    /* 
	     * Spooling is only activated after (a) having processed
	     * the headers, and (b) after the wait command has
	     * required to spool. Once we know spoolFd, there is no
	     * need to HttpCheckHeader() again.
	     */
	    if (httpPtr->spoolFd > 0) {
		Ns_HttpAppendBuffer(httpPtr, buf, n);
	    } else {
		Ns_Log(Ns_LogTaskDebug, "Task got %d bytes", (int)n);
		Ns_HttpAppendBuffer(httpPtr, buf, n);

		if (unlikely(httpPtr->replyHeaderSize == 0)) {
		    Ns_HttpCheckHeader(httpPtr);
		}
		/*
		 * Ns_HttpCheckSpool might set httpPtr->spoolFd
		 */
		Ns_HttpCheckSpool(httpPtr);
		/*Ns_Log(Ns_LogTaskDebug, "Task got %d bytes, header = %d", (int)n, httpPtr->replyHeaderSize);*/
	    }
            return;
        }
        if (n < 0) {
            httpPtr->error = "recv failed";
        }
        break;

    case NS_SOCK_DONE:
        return;

    case NS_SOCK_TIMEOUT:
        httpPtr->error = "timeout";
        break;

    case NS_SOCK_EXIT:
        httpPtr->error = "shutdown";
        break;

    case NS_SOCK_CANCEL:
        httpPtr->error = "cancelled";
        break;

    case NS_SOCK_EXCEPTION:
	httpPtr->error = "exception";
	break;
    }

    /*
     * Get completion time and mark task as done.
     */

    Ns_GetTime(&httpPtr->etime);
    Ns_TaskDone(httpPtr->task);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
