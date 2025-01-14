/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2001-2012 Vlad Seryakov
 * Copyright (C) 2012-2018 Gustaf Neumann
 * All rights reserved.
 *
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

/* Needed for SSL support on Windows: */
#if defined(_MSC_VER) && !defined(HAVE_CONFIG_H)
# include "nsconfig-win32.h"
#endif

#include "ns.h"

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

/* Start:HAVE_OPENSSL_EVP_H: Big ifdef block that covers most of this file. */
#ifdef HAVE_OPENSSL_EVP_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "../nsd/nsopenssl.h"

#define NSSSL_VERSION  "2.3"

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
static Ns_DriverConnInfoProc ConnInfo;
static Ns_DriverCloseProc Close;
static Ns_DriverClientInitProc ClientInit;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static void SSLLock(int mode, int n, const char *file, int line);
static unsigned long SSLThreadId(void);
#endif

/*
 * Static variables defined in this file.
 */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static Ns_Mutex *driver_locks = NULL;
#endif

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    Tcl_DString        ds;
    int                num, result;
    const char        *path, *vhostcertificates;
    NsSSLConfig       *drvCfgPtr;
    Ns_DriverInitData  init;

    memset(&init, 0, sizeof(init));
    Tcl_DStringInit(&ds);

    path = Ns_ConfigSectionPath(NULL, server, module, (char *)0L);
    drvCfgPtr = NsSSLConfigNew(path);

    init.version = NS_DRIVER_VERSION_5;
    init.name = "nsssl";
    init.listenProc = Listen;
    init.acceptProc = Accept;
    init.recvProc = Recv;
    init.sendProc = Send;
    init.sendFileProc = NULL;
    init.keepProc = Keep;
    init.connInfoProc = ConnInfo;
    init.requestProc = NULL;
    init.closeProc = Close;
    init.clientInitProc = ClientInit;
    init.opts = NS_DRIVER_SSL|NS_DRIVER_ASYNC;
    init.arg = drvCfgPtr;
    init.path = path;
    init.protocol = "https";
    init.defaultPort = 443;
#ifdef OPENSSL_VERSION_TEXT
    init.libraryVersion = OPENSSL_VERSION_TEXT;
#else
    init.libraryVersion = ns_strdup(SSLeay_version(SSLEAY_VERSION));
#endif

    /*
     * In case "vhostcertificates" was specified in the configuration file,
     * and it is valid, activate NS_DRIVER_SNI.
     */
    vhostcertificates = Ns_ConfigGetValue(path, "vhostcertificates");
    if (vhostcertificates != NULL && *vhostcertificates != '\0') {
        struct stat st;

        if (stat(vhostcertificates, &st) != 0) {
            Ns_Log(Warning, "vhostcertificates directory '%s' does not exist",
                   vhostcertificates);

        } else if (S_ISDIR(st.st_mode) == 0) {
            Ns_Log(Warning, "value specified for vhostcertificates is not a directory: '%s'",
                   vhostcertificates);

        } else {
            Ns_Log(Notice, "vhostcertificates directory '%s' is valid, activating SNI",
                   vhostcertificates);
            init.opts |= NS_DRIVER_SNI;
        }
    }

    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "nsssl: driver init failed.");
        ns_free(drvCfgPtr);
        return NS_ERROR;
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    num = CRYPTO_num_locks();
    driver_locks = ns_calloc((size_t)num, sizeof(*driver_locks));
    {   int n;
        for (n = 0; n < num; n++) {
            Ns_DStringPrintf(&ds, "nsssl:%s:%d", module, n);
            Ns_MutexSetName(driver_locks + n, ds.string);
            Tcl_DStringSetLength(&ds, 0);
        }
    }

    CRYPTO_set_locking_callback(SSLLock);
    CRYPTO_set_id_callback(SSLThreadId);
#endif
    Ns_Log(Notice, "nsssl: OpenSSL %s initialized", SSLeay_version(SSLEAY_VERSION));

    result = Ns_TLS_CtxServerInit(path, NULL, NS_DRIVER_SNI, drvCfgPtr, &drvCfgPtr->ctx);
    if (result != TCL_OK) {
        Ns_Log(Error, "nsssl: could not initialize OpenSSL context (section %s): %s", path, strerror(errno));
        return NS_ERROR;
    }

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
    Ns_Log(Notice, "nsssl: version %s loaded, based on %s",
           NSSSL_VERSION, init.libraryVersion);
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Listen --
 *
 *      Open a listening socket in nonblocking mode.
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
    bool      unixDomainSocket;

    unixDomainSocket = (*address == '/');

    if (unixDomainSocket) {
        Ns_Log(Error, "nsssl driver does not support unix domain socket: unix:%s", address);
        sock = NS_INVALID_SOCKET;
    } else {
        sock = Ns_SockListenEx(address, port, backlog, reuseport);
        if (sock != NS_INVALID_SOCKET) {
            const NsSSLConfig *drvCfgPtr = driver->arg;

            (void) Ns_SockSetNonBlocking(sock);
            if (drvCfgPtr->deferaccept) {
                Ns_SockSetDeferAccept(sock, (long)driver->recvwait.sec);
            }

            Ns_Log(Notice, "listening on [%s]:%d (sock %d)", address, port, (int)sock);
        }
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Accept --
 *
 *      Accept a new socket in nonblocking mode.
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
    NsSSLConfig *drvCfgPtr = sock->driver->arg;
    SSLContext  *sslCtx = sock->arg;

    sock->sock = Ns_SockAccept(listensock, sockaddrPtr, socklenPtr);

    if (sock->sock != NS_INVALID_SOCKET) {
#ifdef __APPLE__
      /*
       * Darwin's poll returns per default writable in situations,
       * where nothing can be written.  Setting the socket option for
       * the send low watermark to 1 fixes this problem.
       */
        int value = 1;
        setsockopt(sock->sock, SOL_SOCKET, SO_SNDLOWAT, &value, sizeof(value));
#endif
        (void)Ns_SockSetNonBlocking(sock->sock);
        if (drvCfgPtr->nodelay != 0) {
            Ns_SockSetNodelay(sock->sock);
        }

        if (sslCtx == NULL) {
            sslCtx = ns_calloc(1, sizeof(SSLContext));
            sslCtx->ssl = SSL_new(drvCfgPtr->ctx);
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
            SSL_set_accept_state(sslCtx->ssl);
            SSL_set_app_data(sslCtx->ssl, sock);
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
    const NsSSLConfig *drvCfgPtr = sock->driver->arg;
    SSLContext        *sslCtx = sock->arg;
    Ns_SockState       sockState = NS_SOCK_NONE;
    ssize_t            nRead = 0;
    unsigned long      sslERRcode = 0u;
    /*
     * Verify client certificate, driver may require valid cert
     */

    if (drvCfgPtr->verify && sslCtx->verified == 0) {
        X509 *peer;
#ifdef HAVE_OPENSSL_3
        peer = SSL_get0_peer_certificate(sslCtx->ssl);
#else
        peer = SSL_get_peer_certificate(sslCtx->ssl);
#endif
        if (peer != NULL) {
#ifndef HAVE_OPENSSL_3
            X509_free(peer);
#endif
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
        nRead = Ns_SSLRecvBufs2(sslCtx->ssl, bufs, nbufs, &sockState, &sslERRcode);
    }
    Ns_SockSetReceiveState(sock, sockState, sslERRcode);

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
Send(Ns_Sock *sock, const struct iovec *bufs, int nbufs, unsigned int UNUSED(flags))
{
    SSLContext *sslCtx = sock->arg;
    ssize_t     sent = 0;

    if (sslCtx == NULL) {
        Ns_Log(Warning, "nsssl Send is called on a socket without sslCtx (sock %d)",
               sock->sock);
        sent = -1;
    } else {
        bool decork = Ns_SockCork(sock, NS_TRUE);

        while (nbufs > 0) {
            if (bufs->iov_len > 0) {
                int rc;

                ERR_clear_error();
                rc = SSL_write(sslCtx->ssl, bufs->iov_base, (int)bufs->iov_len);
                if (rc <= 0) {
                    int sslerr = SSL_get_error(sslCtx->ssl, rc);

                    /*fprintf(stderr,
                      "### SSL_write %p len %d rc %d SSL_get_error => %d: %s\n",
                      (void*)bufs->iov_base, (int)bufs->iov_len,
                      rc, sslerr, ERR_error_string(ERR_get_error(), NULL));*/

                    if (sslerr == SSL_ERROR_WANT_WRITE) {

                        /*
                         * Effectively, SSL_ERROR_WANT_WRITE at this place
                         * means we are against EWOULDBLOCK, so exit early,
                         * reporting so much bytes sent as we did so far.
                         */

                        break;
                    }

                    SSL_set_shutdown(sslCtx->ssl, SSL_RECEIVED_SHUTDOWN);

                    sent = -1;
                    Ns_SockSetSendErrno(sock, (unsigned int)sslerr);

                    break; /* Any other error case is terminal */
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
        if (likely(bio != NULL)) {
            int flush = BIO_flush(bio);
            if (likely(flush == 1)) {
                return NS_TRUE;
            }
        }
    }
    /*fprintf(stderr, "##### Keep (%d) => 0\n", sock->sock);*/
    return NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnInfo --
 *
 *      Return Tcl_Obj hinting connection details
 *
 * Results:
 *      Tcl_Obj *
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj*
ConnInfo(Ns_Sock *sock)
{
    SSLContext *sslCtx;
    Tcl_Obj    *resultObj;
    char        ipString[NS_IPADDR_SIZE];
    const struct sockaddr *ipPtr;

    NS_NONNULL_ASSERT(sock != NULL);

    resultObj = Tcl_NewDictObj();
    sslCtx = sock->arg;
    ipPtr = Ns_SockGetConfiguredSockAddr(sock);
    (void)ns_inet_ntop(ipPtr, ipString, NS_IPADDR_SIZE);

    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("currentaddr", 11),
                   Tcl_NewStringObj(ipString, -1));

    (void)Ns_SockaddrAddToDictIpProperties(ipPtr, resultObj);

    /*Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("protocol", 8),
                   Tcl_NewStringObj(sock->driver->protocol, TCL_INDEX_NONE));*/
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("sslversion", 10),
                   Tcl_NewStringObj(SSL_get_version(sslCtx->ssl), TCL_INDEX_NONE));
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("cipher", 6),
                   Tcl_NewStringObj(SSL_get_cipher(sslCtx->ssl), TCL_INDEX_NONE));
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("servername", 10),
                   Tcl_NewStringObj(SSL_get_servername(sslCtx->ssl, TLSEXT_NAMETYPE_host_name), TCL_INDEX_NONE));

    return resultObj;
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

        /*
         * SSL_shutdown() must not be called if a previous fatal error has
         * occurred on a connection i.e. if SSL_get_error() has returned
         * SSL_ERROR_SYSCALL or SSL_ERROR_SSL.
         */
        if (!Ns_SockInErrorState(sock) && SSL_in_init(sslCtx->ssl) != 1) {
            int r = SSL_shutdown(sslCtx->ssl);

            Ns_Log(Debug, "### SSL close(%d) shutdown returned %d err %d",
                   SSL_get_fd(sslCtx->ssl), r, SSL_get_error(sslCtx->ssl, r));

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
        } else if (Ns_SockInErrorState(sock)) {
            Ns_Log(Debug, "### SSL close(%d) avoid shutdown in error state",
                   SSL_get_fd(sslCtx->ssl));
        }
        SSL_free(sslCtx->ssl);
        ns_free(sslCtx);
    }
    if (sock->sock > -1) {
        Ns_Log(Debug, "### SSL close(%d) socket", sock->sock);
        ns_sockclose(sock->sock);
        sock->sock = -1;
    }
    sock->arg = NULL;
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
 *        Initialize client connection for the already open sockPtr.
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
ClientInit(Tcl_Interp *interp, Ns_Sock *sockPtr, void *arg)
{
    SSL                    *ssl;
    Ns_DriverClientInitArg *params= (Ns_DriverClientInitArg *)arg;
    int                     result;

    if (Ns_TLS_SSLConnect(interp, sockPtr->sock,
                          params->ctx,
                          params->sniHostname,
                          NULL,
                          &ssl) == NS_OK) {
        SSLContext *sslCtx = ns_calloc(1, sizeof(SSLContext));

        sslCtx->ssl = ssl;
        sockPtr->arg = sslCtx;
        result = TCL_OK;
    } else {
        if (ssl != NULL) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        result = TCL_ERROR;
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
/* End: HAVE_OPENSSL_EVP_H: Big ifdef block that covers most of this file. */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
