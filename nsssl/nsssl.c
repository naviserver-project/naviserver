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
 * Copyright (C) 2012-2018 Gustaf Neumann
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

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

#ifdef HAVE_OPENSSL_EVP_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define NSSSL_VERSION  "2.1"

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

/*
 * Local functions defined in this file
 */

static Ns_DriverListenProc Listen;
static Ns_DriverAcceptProc Accept;
static Ns_DriverRecvProc Recv;
static Ns_DriverSendProc Send;
static Ns_DriverKeepProc Keep;
static Ns_DriverCloseProc Close;
static Ns_DriverClientInitProc ClientInit;

static int SSLPassword(char *buf, int num, int rwflag, void *userdata);

static DH *SSL_dhCB(SSL *ssl, int isExport, int keyLength);

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static void SSLLock(int mode, int n, const char *file, int line);
static unsigned long SSLThreadId(void);
#endif

/*
 * Static variables defined in this file.
 */

static Ns_Mutex *driver_locks;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;


#if OPENSSL_VERSION_NUMBER < 0x10100000L
/*
 * The renegotiation issue was fixed in recent versions of OpenSSL,
 * and the flag was removed.
 */
static void
SSL_infoCB(const SSL *ssl, int where, int UNUSED(ret)) {
    if ((where & SSL_CB_HANDSHAKE_DONE)) {

        ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
    }
}
#endif


/*
 * Include pre-generated DH parameters
 */
#ifndef HEADER_DH_H
#include <openssl/dh.h>
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
 * Callback used for ephemeral DH keys
 */
static DH *
SSL_dhCB(SSL *ssl, int isExport, int keyLength) {
    SSLDriver *drvPtr;
    DH *key;

    Ns_Log(Debug, "SSL_dhCB: isExport %d keyLength %d", isExport, keyLength);
    drvPtr = (SSLDriver *) SSL_get_app_data(ssl);

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

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    Tcl_DString         ds;
    int                num;
    const char        *path, *value;
    SSLDriver         *drvPtr;
    Ns_DriverInitData  init;

    memset(&init, 0, sizeof(init));
    Tcl_DStringInit(&ds);

    path = Ns_ConfigGetPath(server, module, (char *)0L);

    drvPtr = ns_calloc(1, sizeof(SSLDriver));
    drvPtr->deferaccept = Ns_ConfigBool(path, "deferaccept", NS_FALSE);
    drvPtr->verify = Ns_ConfigBool(path, "verify", 0);

    init.version = NS_DRIVER_VERSION_4;
    init.name = "nsssl";
    init.listenProc = Listen;
    init.acceptProc = Accept;
    init.recvProc = Recv;
    init.sendProc = Send;
    init.sendFileProc = NULL;
    init.keepProc = Keep;
    init.requestProc = NULL;
    init.closeProc = Close;
    init.clientInitProc = ClientInit;
    init.opts = NS_DRIVER_SSL|NS_DRIVER_ASYNC;
    init.arg = drvPtr;
    init.path = path;
    init.protocol = "https";
    init.defaultPort = 443;

    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "nsssl: driver init failed.");
        ns_free(drvPtr);
        return NS_ERROR;
    }

    num = CRYPTO_num_locks();
    driver_locks = ns_calloc((size_t)num, sizeof(*driver_locks));
    {   int n;
        for (n = 0; n < num; n++) {
            Ns_DStringPrintf(&ds, "nsssl:%s:%d", module, n);
            Ns_MutexSetName(driver_locks + n, ds.string);
            Tcl_DStringSetLength(&ds, 0);
        }
    }
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    CRYPTO_set_locking_callback(SSLLock);
    CRYPTO_set_id_callback(SSLThreadId);
#endif
    Ns_Log(Notice, "OpenSSL %s initialized", SSLeay_version(SSLEAY_VERSION));

    drvPtr->ctx = SSL_CTX_new(SSLv23_server_method());
    if (drvPtr->ctx == NULL) {
        Ns_Log(Error, "nsssl: init error: %s", strerror(errno));
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
        Ns_Log(Error, "nsssl: certificate parameter must be specified in the config file under %s", path);
        return NS_ERROR;
    }

    if (SSL_CTX_use_certificate_chain_file(drvPtr->ctx, value) != 1) {
        Ns_Log(Error, "nsssl: certificate load error from cert %s: %s", value, ERR_error_string(ERR_get_error(), NULL));
        return NS_ERROR;
    }
    if (SSL_CTX_use_PrivateKey_file(drvPtr->ctx, value, SSL_FILETYPE_PEM) != 1) {
        Ns_Log(Error, "nsssl: private key load error: %s", ERR_error_string(ERR_get_error(), NULL));
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
        SSL_CTX_set_options(drvPtr->ctx, SSL_OP_SINGLE_ECDH_USE);
        if (SSL_CTX_set_tmp_ecdh(drvPtr->ctx, ecdh) != 1) {
            Ns_Log(Error, "nsssl: Couldn't set ecdh parameters");
            return NS_ERROR;
        }
        EC_KEY_free (ecdh);
    }
#endif

    /*
     * Https cache support
     */
    Ns_DStringPrintf(&ds, "nsssl:%d", getpid());
    SSL_CTX_set_session_id_context(drvPtr->ctx, (void *) ds.string, (unsigned int)ds.length);
    SSL_CTX_set_session_cache_mode(drvPtr->ctx, SSL_SESS_CACHE_SERVER);

    /*
     * Parse SSL protocols
     */
    {
        unsigned long n = SSL_OP_ALL;

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
                /*
                 * Currently, we can't deselect v1.1 or v1.2 or 1.3 using
                 * SSL_OP_NO_TLSv1_1 etc., but just the full "TLSv1" block.
                 */

                n |= SSL_OP_NO_TLSv1;
                Ns_Log(Notice, "nsssl: disabling TLSv1");
            }
        }
        SSL_CTX_set_options(drvPtr->ctx, n);
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    /*
     * Set info callback to prevent client-initiated renegotiation
     * (after the handshake).
     */
    SSL_CTX_set_info_callback(drvPtr->ctx, SSL_infoCB);
#endif

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
    Tcl_DStringSetLength(&ds, 1024);
    for (num = 0; !RAND_status() && num < 3; num++) {
        int n;

        Ns_Log(Notice, "nsssl: Seeding OpenSSL's PRNG");
        for (n = 0; n < 1024; n++) {
            ds.string[n] = (char)(Ns_DRand()*255);
        }
        RAND_seed(ds.string, 1024);
    }
    if (!RAND_status()) {
        Ns_Log(Warning, "nsssl: PRNG fails to have enough entropy");
    }

    Tcl_DStringFree(&ds);
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
Listen(Ns_Driver *driver, const char *address, unsigned short port, int backlog, bool reuseport)
{
    NS_SOCKET sock;

    sock = Ns_SockListenEx(address, port, backlog, reuseport);
    if (sock != NS_INVALID_SOCKET) {
        SSLDriver *cfg = driver->arg;

        (void) Ns_SockSetNonBlocking(sock);
        if (cfg->deferaccept) {
            Ns_SockSetDeferAccept(sock, driver->recvwait.sec);
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
    SSLDriver  *drvPtr = sock->driver->arg;
    SSLContext *sslCtx = sock->arg;

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
        (void)Ns_SockSetNonBlocking(sock->sock);

        if (sslCtx == NULL) {
            sslCtx = ns_calloc(1, sizeof(SSLContext));
            sslCtx->ssl = SSL_new(drvPtr->ctx);
            if (sslCtx->ssl == NULL) {
                char ipString[NS_IPADDR_SIZE];
                Ns_Log(Error, "%d: SSL session init error for %s: [%s]",
                       sock->sock,
                       ns_inet_ntop((struct sockaddr *)&(sock->sa), ipString, sizeof(ipString)),
                       strerror(errno));
                ns_free(sslCtx);
                return NS_DRIVER_ACCEPT_ERROR;
            }
            sock->arg = sslCtx;
            SSL_set_fd(sslCtx->ssl, sock->sock);
            SSL_set_mode(sslCtx->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
            SSL_set_accept_state(sslCtx->ssl);
            SSL_set_app_data(sslCtx->ssl, drvPtr);
            SSL_set_tmp_dh_callback(sslCtx->ssl, SSL_dhCB);
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
 *      Total number of bytes received or -1 on error. The return
 *      value will be 0 when the peer has performed an orderly shutdown. The
 *      resulting sockstate has one of the following codes:
 *
 *      NS_SOCK_READ, NS_SOCK_DONE, NS_SOCK_AGAIN, NS_SOCK_EXCEPTION
 *
 *      No NS_SOCK_TIMEOUT handling
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Recv(Ns_Sock *sock, struct iovec *bufs, int nbufs,
     Ns_Time *UNUSED(timeoutPtr), unsigned int UNUSED(flags))
{
    SSLDriver   *drvPtr = sock->driver->arg;
    SSLContext  *sslCtx = sock->arg;
    Ns_SockState sockState = NS_SOCK_NONE;
    ssize_t      nRead = 0;

    /*
     * Verify client certificate, driver may require valid cert
     */

    if (drvPtr->verify && sslCtx->verified == 0) {
        X509 *peer;

        peer = SSL_get_peer_certificate(sslCtx->ssl);

        if (peer != NULL) {
            X509_free(peer);
            if (SSL_get_verify_result(sslCtx->ssl) != X509_V_OK) {
                char ipString[NS_IPADDR_SIZE];

                Ns_Log(Error, "nsssl: client certificate not valid by %s",
                       ns_inet_ntop((struct sockaddr *)&(sock->sa), ipString,
                                    sizeof(ipString)));
                nRead = -1;
                sockState = NS_SOCK_EXCEPTION;
            }
        } else {
            char ipString[NS_IPADDR_SIZE];

            Ns_Log(Error, "nsssl: no client certificate provided by %s",
                   ns_inet_ntop((struct sockaddr *)&(sock->sa), ipString,
                                sizeof(ipString)));
            nRead = -1;
            sockState = NS_SOCK_EXCEPTION;
        }
        sslCtx->verified = 1;
    }

    if (nRead > -1) {
        nRead = Ns_SSLRecvBufs2(sslCtx->ssl, bufs, nbufs, &sockState);
    }

    Ns_SockSetReceiveState(sock, sockState);

    return nRead;
}


/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Send data from given buffers.
 *
 * Results:
 *      Total number of bytes sent, -1 on error.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Send(Ns_Sock *sock, const struct iovec *bufs, int nbufs,
     const Ns_Time *UNUSED(timeoutPtr), unsigned int UNUSED(flags))
{
    SSLContext *sslCtx = sock->arg;
    ssize_t     sent = 0;
    bool        decork;

    decork = Ns_SockCork(sock, NS_TRUE);

    while (nbufs > 0) {
        if (bufs->iov_len > 0) {
            int rc;
            ERR_clear_error();
            rc = SSL_write(sslCtx->ssl, bufs->iov_base, (int)bufs->iov_len);
            if (rc <= 0) {
                /*fprintf(stderr, "### SSL_write %p len %d rc %d SSL_get_error => %d: %s\n",
                        (void*)bufs->iov_base, (int)bufs->iov_len,
                        rc, SSL_get_error(sslCtx->ssl, rc),
                        ERR_error_string(ERR_get_error(), NULL));*/
                if (SSL_get_error(sslCtx->ssl, rc) != SSL_ERROR_WANT_WRITE) {
                    SSL_set_shutdown(sslCtx->ssl, SSL_RECEIVED_SHUTDOWN);
                    sent = -1;
                } else {
                    sent = 0;
                }
                break;
            }
            sent += (ssize_t)rc;
            if (rc < (int)bufs->iov_len) {
                Ns_Log(Debug, "SSL: partial write, wanted %" PRIuz " wrote %d",
                       bufs->iov_len, rc);
                break;
            }
        }
        nbufs--;
        bufs++;
    }

    if (decork) {
        Ns_SockCork(sock, NS_FALSE);
    }

    return sent;
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
    SSLContext *sslCtx = sock->arg;

    if (SSL_get_shutdown(sslCtx->ssl) == 0) {
        BIO *bio = SSL_get_wbio(sslCtx->ssl);
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
    SSLContext *sslCtx = sock->arg;

    if (sslCtx != NULL) {
        int r;

        /*
         * SSL_shutdown() must not be called if a previous fatal error has
         * occurred on a connection i.e. if SSL_get_error() has returned
         * SSL_ERROR_SYSCALL or SSL_ERROR_SSL.
         */
        if (!Ns_SockInErrorState(sock)) {
            int fd = SSL_get_fd(sslCtx->ssl);

            r = SSL_shutdown(sslCtx->ssl);
            Ns_Log(Debug, "### SSL close(%d) err %d", fd, SSL_get_error(sslCtx->ssl, r));

            if (r == 0) {
                /*
                 * The first shutdown did not work, so try again. However, to be
                 * sure that SSL_shutdown() does not block, issue a socket
                 * shutdown() command first.
                 */
                shutdown(SSL_get_fd(sslCtx->ssl), SHUT_RDWR);
                r = SSL_shutdown(sslCtx->ssl);
            }
            if (r == -1) {
                unsigned long err = ERR_get_error();

                if (err != 0) {
                    char errorBuffer[256];

                    Ns_Log(Notice, "SSL_shutdown(%d) has failed: %s",
                           sock->sock, ERR_error_string(err, errorBuffer));
                }
            }
        } else {
            Ns_Log(Notice, "### SSL close(%d) avoid shutdown in error state",
                   SSL_get_fd(sslCtx->ssl));
        }
        SSL_free(sslCtx->ssl);
        ns_free(sslCtx);
    }
    if (sock->sock > -1) {
        Ns_Log(Debug, "### SSL close(%d) socket",sock->sock);
        ns_sockclose(sock->sock);
        sock->sock = -1;
    }
    sock->arg = NULL;
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

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static void
SSLLock(int mode, int n, const char *UNUSED(file), int UNUSED(line))
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
#endif


/*
 *----------------------------------------------------------------------
 *
 * ClientInit --
 *
 *        Initialize client connection for the already open spockPtr.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        None
 *
 *----------------------------------------------------------------------
 */
static int
ClientInit(Tcl_Interp *interp, Ns_Sock *sockPtr, NS_TLS_SSL_CTX *ctx)
{
    SSL          *ssl;
    SSLContext   *sslCtx;
    int           result;

    result = Ns_TLS_SSLConnect(interp, sockPtr->sock, ctx, NULL, &ssl);

    if (likely(result == TCL_OK)) {
        sslCtx = ns_calloc(1, sizeof(SSLContext));
        sslCtx->ssl = ssl;
        sockPtr->arg = sslCtx;
    } else if (ssl != NULL) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }

    return result;
}
#else

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    Ns_Log(Warning, "modules nsssl requires a version of NaviServer built with OpenSSL");
    return NS_ERROR;
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
