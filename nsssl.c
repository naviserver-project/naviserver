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
 */

#include "ns.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define NSSSL_VERSION  "0.2"

typedef struct {
    SSL_CTX     *ctx;
    Ns_Mutex     lock;
    int          verify;
    int          deferaccept;  /* Enable the TCP_DEFER_ACCEPT optimization. */
} SSLDriver;

typedef struct {
    SSL         *ssl;
    int          verified;
} SSLContext;

typedef struct {
    int          sock;
    size_t       len;
    int          status;
    Ns_Time      timeout;
    Ns_Time      stime;
    Ns_Time      etime;
    Ns_Task     *task;
    char        *url;
    char        *error;
    char        *next;
    Tcl_DString  ds;
    SSL_CTX     *ctx;
    SSL         *ssl;
} Session;

/*
 * Local functions defined in this file
 */

static Ns_DriverListenProc Listen;
static Ns_DriverAcceptProc Accept;
static Ns_DriverRecvProc Recv;
static Ns_DriverSendProc Send;
static Ns_DriverKeepProc Keep;
static Ns_DriverCloseProc Close;

static int SSLInterpInit(Tcl_Interp *interp, void *arg);
static int SSLObjCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);
static int SSLPassword(char *buf, int num, int rwflag, void *userdata);
static void SSLLock(int mode, int n, const char *file, int line);
static unsigned long SSLThreadId(void);


static Tcl_Obj *SessionResult(Tcl_DString *ds, int *statusPtr, Ns_Set *hdrs);
static void SessionClose(Session *sesPtr);
static void SessionCancel(Session *sesPtr);
static void SessionAbort(Session *sesPtr);
static int SessionSetVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr);
static Session *SessionGet(Tcl_Interp *interp, char *id);
static Ns_TaskProc SessionProc;

static void SetDeferAccept(Ns_Driver *driver, NS_SOCKET sock);

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

NS_EXPORT int
Ns_ModuleInit(char *server, char *module)
{
    Ns_DString ds;
    int num, n;
    char *path, *value;
    SSLDriver *drvPtr;
    Ns_DriverInitData init;

    Ns_DStringInit(&ds);

    path = Ns_ConfigGetPath(server, module, NULL);

    drvPtr = ns_calloc(1, sizeof(SSLDriver));
    drvPtr->deferaccept = Ns_ConfigBool(path, "deferaccept", NS_FALSE);
    drvPtr->verify = Ns_ConfigBool(path, "verify", 0);

    init.version = NS_DRIVER_VERSION_2;
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
    init.path = (char*)path;

    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "nsssl: driver init failed.");
        ns_free(drvPtr);
        return NS_ERROR;
    }

    num = CRYPTO_num_locks();
    driver_locks = ns_calloc(num, sizeof(*driver_locks));
    for (n = 0; n < num; n++) {
        Ns_DStringPrintf(&ds, "nsssl:%d", n);
        Ns_MutexSetName(driver_locks + n, ds.string);
        Ns_DStringTrunc(&ds, 0);
    }
    CRYPTO_set_locking_callback(SSLLock);
    CRYPTO_set_id_callback(SSLThreadId);
    CRYPTO_set_mem_functions(ns_malloc, ns_realloc, ns_free);

    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    SSL_library_init();

    drvPtr->ctx = SSL_CTX_new(SSLv23_server_method());
    if (drvPtr->ctx == NULL) {
        Ns_Log(Error, "nsssl: init error [%s]",strerror(errno));
        return NS_ERROR;
    }

    /* 
     * Load certificate and private key 
     */
    value = Ns_ConfigGetValue(path, "certificate");
    if (value == NULL) {
        Ns_Log(Error, "nsssl: certificate parameter should be specified under %s",path);
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
     * Session cache support
     */
    Ns_DStringPrintf(&ds, "nsssl:%d", getpid());
    SSL_CTX_set_session_id_context(drvPtr->ctx, (void *) ds.string, ds.length);
    SSL_CTX_set_session_cache_mode(drvPtr->ctx, SSL_SESS_CACHE_SERVER);

    /*
     * Parse SSL protocols
     */
    n = SSL_OP_ALL;
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
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
    /*
     * Prefer server ciphers to secure against BEAST attack.
     */
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    /*
     * Disable compression to avoid CRIME attack.
     */
#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_NO_COMPRESSION);
#endif
    if (drvPtr->verify) {
        SSL_CTX_set_verify(drvPtr->ctx, SSL_VERIFY_PEER, NULL);
    }

    /*
     * Seed the OpenSSL Pseudo-Random Number Generator.
     */
    Ns_DStringSetLength(&ds, 1024);
    for (num = 0; !RAND_status() && num < 3; num++) {
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
    Ns_Log(Notice, "nsssl: version %s loaded", NSSSL_VERSION);
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
 *      The open socket or INVALID_SOCKET on error.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static SOCKET
Listen(Ns_Driver *driver, CONST char *address, int port, int backlog)
{
    SOCKET sock;

    sock = Ns_SockListenEx((char*)address, port, backlog);
    if (sock != INVALID_SOCKET) {
        (void) Ns_SockSetNonBlocking(sock);
        SetDeferAccept(driver, sock);
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
Accept(Ns_Sock *sock, SOCKET listensock, struct sockaddr *sockaddrPtr, int *socklenPtr)
{
    SSLDriver *drvPtr = sock->driver->arg;
    SSLContext *sslPtr = sock->arg;

    sock->sock = Ns_SockAccept(listensock, sockaddrPtr, socklenPtr);
    if (sock->sock != INVALID_SOCKET) {
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
                Ns_Log(Error, "%d: SSL session init error for %s: [%s]", 
		       sock->sock, 
		       ns_inet_ntoa(sock->sa.sin_addr), 
		       strerror(errno));
                ns_free(sslPtr);
                return NS_DRIVER_ACCEPT_ERROR;
            }
            sock->arg = sslPtr;
            SSL_set_fd(sslPtr->ssl, sock->sock);
            SSL_set_accept_state(sslPtr->ssl);
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
Recv(Ns_Sock *sock, struct iovec *bufs, int nbufs, Ns_Time *timeoutPtr, int flags)
{
    SSLDriver *drvPtr = sock->driver->arg;
    SSLContext *sslPtr = sock->arg;
    X509 *peer;
    int err, n;

    /*
     * Verify client certificate, driver may require valid cert
     */

    if (drvPtr->verify && sslPtr->verified == 0) {
        if ((peer = SSL_get_peer_certificate(sslPtr->ssl))) {
             X509_free(peer);
             if (SSL_get_verify_result(sslPtr->ssl) != X509_V_OK) {
                 Ns_Log(Error, "nsssl: client certificate not valid by %s", 
			ns_inet_ntoa(sock->sa.sin_addr));
                 return NS_ERROR;
             }
        } else {
            Ns_Log(Error, "nsssl: no client certificate provided by %s", 
		   ns_inet_ntoa(sock->sa.sin_addr));
            return NS_ERROR;
        }
        sslPtr->verified = 1;
    }

    while (1) {
        char *p = (char *)bufs->iov_base;
	int   got = 0;
	
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
Send(Ns_Sock *sock, struct iovec *bufs, int nbufs, Ns_Time *timeoutPtr, int flags)
{
    SSLContext *sslPtr = sock->arg;
    int rc, size, decork;

    size = 0;
    decork = Ns_SockCork(sock, 1);
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
		if (decork) {Ns_SockCork(sock->sock, 0);}
		SSL_set_shutdown(sslPtr->ssl, SSL_RECEIVED_SHUTDOWN);
		return -1;
	    }
	    size += rc;
	}
	nbufs--;
	bufs++;
    }

    if (decork) {Ns_SockCork(sock->sock, 0);}
    return size;
}


/*
 *----------------------------------------------------------------------
 *
 * Keep --
 *
 *      Mo keepalives
 *
 * Results:
 *      0, always.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Keep(Ns_Sock *sock)
{
    SSLContext *sslPtr = sock->arg;

    if (SSL_get_shutdown(sslPtr->ssl) == 0) {
        BIO *bio = SSL_get_wbio(sslPtr->ssl);
        if (bio != NULL && BIO_flush(bio) == 1) {
            return 1;
        }
    }
    return 0;
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
SSLInterpInit(Tcl_Interp *interp, void *arg)
{
    Tcl_CreateObjCommand(interp, "ns_ssl", SSLObjCmd, arg, NULL);
    return NS_OK;
}

static int
SSLPassword(char *buf, int num, int rwflag, void *userdata)
{
    fprintf(stdout, "Enter SSL password:");
    fgets(buf, num, stdin);
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
    Session *sesPtr = NULL;
    Ns_Set *hdrPtr = NULL;
    Ns_Time *timeoutPtr = NULL;
    int i, opt, flag, run = 0;

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
        int sock, len, uaFlag = -1, verify = 0;
        char *key, *body, *host, *file, *port, *cert = NULL;
        char buf[32], *url = NULL, *method = "GET", *caFile = NULL, *caPath = NULL;
        Tcl_Obj *bodyPtr = NULL;

        Ns_ObjvSpec opts[] = {
            {"-timeout",  Ns_ObjvTime,    &timeoutPtr,  NULL},
            {"-method",   Ns_ObjvString,  &method,      NULL},
            {"-cert",     Ns_ObjvString,  &cert,        NULL},
            {"-cafile",   Ns_ObjvString,  &caFile,      NULL},
            {"-capath",   Ns_ObjvString,  &caPath,      NULL},
            {"-body",     Ns_ObjvObj,     &bodyPtr,     NULL},
            {"-headers",  Ns_ObjvSet,     &hdrPtr,      NULL},
            {"-verify",   Ns_ObjvBool,    &verify,      NULL},
            {NULL, NULL,  NULL, NULL}
        };
        Ns_ObjvSpec args[] = {
            {"url",       Ns_ObjvString,  &url,         NULL},
            {NULL, NULL, NULL, NULL}
        };
        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }

        /*
         * Parse and split url
         */

        if (strncmp(url, "https://", 8) != 0 || url[8] == '\0') {
            Tcl_AppendResult(interp, "invalid url: ", url, NULL);
            return TCL_ERROR;
        }
        host = url + 8;
        file = strchr(host, '/');
        if (file != NULL) {
            *file = '\0';
        }
        port = strchr(host, ':');
        if (port == NULL) {
            flag = 443;
        } else {
            *port = '\0';
            flag = (int) strtol(port+1, NULL, 10);
        }

        /*
         * Connect to the host and allocate session struct
         */

        sock = Ns_SockAsyncConnect(host, flag);
        if (sock == INVALID_SOCKET) {
            Tcl_AppendResult(interp, "connect to ", url, " failed: ", ns_sockstrerror(ns_sockerrno), NULL);
            return TCL_ERROR;
        }

        sesPtr = ns_calloc(1, sizeof(Session));
        sesPtr->sock = sock;
        sesPtr->url = ns_strdup(url);
        Tcl_DStringInit(&sesPtr->ds);

        /*
         *  Restore the url string
         */

        if (port != NULL) {
            *port = ':';
        }
        if (file != NULL) {
            *file = '/';
        }

        /*
         * Now initialize OpenSSL context
         */

        sesPtr->ctx = SSL_CTX_new(SSLv23_client_method());
        if (sesPtr->ctx == NULL) {
            Tcl_AppendResult(interp, "ctx init failed: ", ERR_error_string(ERR_get_error(), NULL), NULL);
            SessionClose(sesPtr);
            return TCL_ERROR;
        }
        SSL_CTX_set_default_verify_paths(sesPtr->ctx);
        SSL_CTX_load_verify_locations (sesPtr->ctx, caFile, caPath);
        SSL_CTX_set_verify(sesPtr->ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
        SSL_CTX_set_mode(sesPtr->ctx, SSL_MODE_AUTO_RETRY);
        SSL_CTX_set_mode(sesPtr->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);

        if (cert != NULL) {
            if (SSL_CTX_use_certificate_chain_file(sesPtr->ctx, cert) != 1) {
                Tcl_AppendResult(interp, "certificate load error: ", ERR_error_string(ERR_get_error(), NULL), NULL);
                SessionClose(sesPtr);
                return NS_ERROR;
            }
            if (SSL_CTX_use_PrivateKey_file(sesPtr->ctx, cert, SSL_FILETYPE_PEM) != 1) {
                Tcl_AppendResult(interp, "private key load error: ", ERR_error_string(ERR_get_error(), NULL), NULL);
                SessionClose(sesPtr);
                return NS_ERROR;
            }
        }

        sesPtr->ssl = SSL_new(sesPtr->ctx);
        if (sesPtr->ssl == NULL) {
            Tcl_AppendResult(interp, "ssl init failed: ", ERR_error_string(ERR_get_error(), NULL), NULL);
            SessionClose(sesPtr);
            return TCL_ERROR;
        }

        SSL_set_fd(sesPtr->ssl, sock);
        SSL_set_connect_state(sesPtr->ssl);

        if (SSL_connect(sesPtr->ssl) <= 0 || sesPtr->ssl->state != SSL_ST_OK) {
            Tcl_AppendResult(interp, "ssl connect failed: ", ERR_error_string(ERR_get_error(), NULL), NULL);
            SessionClose(sesPtr);
            return TCL_ERROR;
        }

        Ns_DStringPrintf(&sesPtr->ds, "%s %s HTTP/1.0\r\n", method, file ? file : "/");

        /*
         * Submit provided headers
         */

        if (hdrPtr != NULL) {
            for (i = 0; i < Ns_SetSize(hdrPtr); i++) {
                key = Ns_SetKey(hdrPtr, i);
                if (uaFlag) {
                    uaFlag = strcasecmp(key, "User-Agent");
                }
                Ns_DStringPrintf(&sesPtr->ds, "%s: %s\r\n", key, Ns_SetValue(hdrPtr, i));
            }
        }

        /*
         * User-Agent header was not supplied, add our own header
         */

        if (uaFlag) {
            Ns_DStringPrintf(&sesPtr->ds, "User-Agent: %s/%s\r\n",
                             Ns_InfoServerName(),
                             Ns_InfoServerVersion());
        }

        /*
         * No keep-alive even in case of HTTP 1.1
         */

        Ns_DStringAppend(&sesPtr->ds, "Connection: close\r\n");
        if (port == NULL) {
            Ns_DStringPrintf(&sesPtr->ds, "Host: %s\r\n", host);
        } else {
            Ns_DStringPrintf(&sesPtr->ds, "Host: %s:%d\r\n", host, flag);
        }

        body = NULL;
        if (bodyPtr != NULL) {
            body = Tcl_GetStringFromObj(bodyPtr, &len);
            if (len == 0) {
                body = NULL;
            }
        }
        if (body != NULL) {
            Ns_DStringPrintf(&sesPtr->ds, "Content-Length: %d\r\n", len);
        }
        Tcl_DStringAppend(&sesPtr->ds, "\r\n", 2);
        if (body != NULL) {
            Tcl_DStringAppend(&sesPtr->ds, body, len);
        }
        sesPtr->next = sesPtr->ds.string;
        sesPtr->len = sesPtr->ds.length;

        Ns_GetTime(&sesPtr->stime);
        sesPtr->timeout = sesPtr->stime;
        if (timeoutPtr != NULL) {
            Ns_IncrTime(&sesPtr->timeout, timeoutPtr->sec, timeoutPtr->usec);
        } else {
            Ns_IncrTime(&sesPtr->timeout, 2, 0);
        }
        sesPtr->task = Ns_TaskCreate(sesPtr->sock, SessionProc, sesPtr);
        if (run) {
            Ns_TaskRun(sesPtr->task);
        } else {
            if (session_queue == NULL) {
                Ns_MasterLock();
                if (session_queue == NULL) {
                    session_queue = Ns_CreateTaskQueue("tclssl");
                }
                Ns_MasterUnlock();
            }
            if (Ns_TaskEnqueue(sesPtr->task, session_queue) != NS_OK) {
                SessionClose(sesPtr);
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
        Tcl_SetHashValue(hPtr, sesPtr);
        Ns_MutexUnlock(&session_lock);

        Tcl_SetResult(interp, buf, TCL_VOLATILE);
        break;
    }

    case HWaitIdx: {
        Tcl_Obj *elapsedPtr = NULL;
        Tcl_Obj *resultPtr = NULL;
        Tcl_Obj *statusPtr = NULL;
        Tcl_Obj *valPtr;
        char *id = NULL;
        Ns_Time diff;

        Ns_ObjvSpec opts[] = {
            {"-timeout",  Ns_ObjvTime, &timeoutPtr,  NULL},
            {"-elapsed",  Ns_ObjvObj,  &elapsedPtr,  NULL},
            {"-result",   Ns_ObjvObj,  &resultPtr,   NULL},
            {"-status",   Ns_ObjvObj,  &statusPtr,   NULL},
            {"-headers",  Ns_ObjvSet,  &hdrPtr,      NULL},
            {NULL, NULL,  NULL, NULL}
        };
        Ns_ObjvSpec args[] = {
            {"id",       Ns_ObjvString, &id, NULL},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }
        if (!(sesPtr = SessionGet(interp, id))) {
            return TCL_ERROR;
        }
        if (Ns_TaskWait(sesPtr->task, timeoutPtr) != NS_OK) {
            SessionCancel(sesPtr);
            Tcl_AppendResult(interp, "timeout waiting for task", NULL);
            return TCL_ERROR;
        }
        if (elapsedPtr != NULL) {
            Ns_DiffTime(&sesPtr->etime, &sesPtr->stime, &diff);
            valPtr = Tcl_NewObj();
            Ns_TclSetTimeObj(valPtr, &diff);
            if (!SessionSetVar(interp, elapsedPtr, valPtr)) {
                SessionClose(sesPtr);
                return TCL_ERROR;
            }
        }
        if (sesPtr->error) {
            Tcl_AppendResult(interp, "ssl failed: ", sesPtr->error, NULL);
            SessionClose(sesPtr);
            return TCL_ERROR;
        }
        valPtr = SessionResult(&sesPtr->ds, &flag, hdrPtr);
        if (statusPtr != NULL && !SessionSetVar(interp, statusPtr, Tcl_NewIntObj(flag))) {
            SessionClose(sesPtr);
            return TCL_ERROR;
        }
        if (resultPtr == NULL) {
            Tcl_SetObjResult(interp, valPtr);
        } else {
            if (!SessionSetVar(interp, resultPtr, valPtr)) {
                SessionClose(sesPtr);
                return TCL_ERROR;
            }
            Tcl_SetBooleanObj(Tcl_GetObjResult(interp), 1);
        }
        SessionClose(sesPtr);
        break;
    }

    case HCancelIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "id");
            return TCL_ERROR;
        }
        if (!(sesPtr = SessionGet(interp, Tcl_GetString(objv[2])))) {
            return TCL_ERROR;
        }
        SessionAbort(sesPtr);
        break;

    case HCleanupIdx:
        Ns_MutexLock(&session_lock);
        hPtr = Tcl_FirstHashEntry(&session_table, &search);
        while (hPtr != NULL) {
            sesPtr = Tcl_GetHashValue(hPtr);
            SessionAbort(sesPtr);
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
            sesPtr = Tcl_GetHashValue(hPtr);
            Tcl_AppendResult(interp, Tcl_GetHashKey(&session_table, hPtr), " ",
                             sesPtr->url, " ",
                             Ns_TaskCompleted(sesPtr->task) ? "done" : "running",
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
 * SessionGet --
 *
 *  Locate and remove the Session struct for a given id.
 *
 * Results:
 *  pointer on success, NULL otherwise.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */

static Session *
SessionGet(Tcl_Interp *interp, char *id)
{
    Session *sesPtr = NULL;
    Tcl_HashEntry *hPtr;

    Ns_MutexLock(&session_lock);
    hPtr = Tcl_FindHashEntry(&session_table, id);
    if (hPtr != NULL) {
        sesPtr = Tcl_GetHashValue(hPtr);
        Tcl_DeleteHashEntry(hPtr);
    } else {
        Tcl_AppendResult(interp, "no such request: ", id, NULL);
    }
    Ns_MutexUnlock(&session_lock);
    return sesPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * SessionSetVar --
 *
 *  Set a variable by name.  Convience routine for for SessionWaitCmd.
 *
 * Results:
 *  1 on success, 0 otherwise.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
SessionSetVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr)
{
    Tcl_Obj *errPtr;

    Tcl_IncrRefCount(valPtr);
    errPtr = Tcl_ObjSetVar2(interp, varPtr, NULL, valPtr, TCL_PARSE_PART1|TCL_LEAVE_ERR_MSG);
    Tcl_DecrRefCount(valPtr);
    return (errPtr ? 1 : 0);
}


/*
 *----------------------------------------------------------------------
 *
 * SessionResult --
 *
 *        Parse an Session response for the result body and headers.
 *
 * Results:
 *        Pointer to body within Session buffer.
 *
 * Side effects:
 *        Will append parsed response headers to given hdrs if
 *        not NULL and set HTTP status code in given statusPtr.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
SessionResult(Tcl_DString *ds, int *statusPtr, Ns_Set *hdrs)
{
    char *eoh, *body, *response;
    int major, minor;
    Tcl_Obj *result;

    body = response = ds->string;
    eoh = strstr(response, "\r\n\r\n");
    if (eoh != NULL) {
        body = eoh + 4;
        eoh += 2;
    } else {
        eoh = strstr(response, "\n\n");
        if (eoh != NULL) {
            body = eoh + 2;
            eoh += 1;
        }
    }

    result = Tcl_NewByteArrayObj((unsigned char*)body, ds->length-(body-response));

    if (eoh == NULL) {
        *statusPtr = 0;
    } else {
        *eoh = '\0';
        sscanf(response, "HTTP/%d.%d %d", &major, &minor, statusPtr);
        if (hdrs != NULL) {
	    char *p, save;
	    int firsthdr;

            save = *body;
            *body = '\0';
            firsthdr = 1;
            p = response;
            while ((eoh = strchr(p, '\n')) != NULL) {
		int len;
                *eoh++ = '\0';
                len = strlen(p);
                if (len > 0 && p[len-1] == '\r') {
                    p[len-1] = '\0';
                }
                if (firsthdr) {
                    if (hdrs->name != NULL) {
                        ns_free(hdrs->name);
                    }
                    hdrs->name = ns_strdup(p);
                    firsthdr = 0;
                } else
                if (Ns_ParseHeader(hdrs, p, ToLower) != NS_OK) {
                    break;
                }
                p = eoh;
            }
            *body = save;
        }
    }
    return result;
}

static void
SessionClose(Session *sesPtr)
{
    if (sesPtr->task != NULL) {
        Ns_TaskFree(sesPtr->task);
    }
    if (sesPtr->ssl != NULL) {
        SSL_shutdown(sesPtr->ssl);
        SSL_free(sesPtr->ssl);
    }
    if (sesPtr->ctx != NULL) {
        SSL_CTX_free(sesPtr->ctx);
    }
    if (sesPtr->sock > 0) {
        ns_sockclose(sesPtr->sock);
    }
    Tcl_DStringFree(&sesPtr->ds);
    ns_free(sesPtr->url);
    ns_free(sesPtr);
}


static void
SessionCancel(Session *sesPtr)
{
    Ns_TaskCancel(sesPtr->task);
    Ns_TaskWait(sesPtr->task, NULL);
}


static void
SessionAbort(Session *sesPtr)
{
    SessionCancel(sesPtr);
    SessionClose(sesPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * SessionProc --
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
SessionProc(Ns_Task *task, SOCKET sock, void *arg, int why)
{
    Session *sesPtr = arg;
    char buf[4096];
    int n;

    switch (why) {
    case NS_SOCK_INIT:
    Ns_TaskCallback(task, NS_SOCK_WRITE, &sesPtr->timeout);
    return;

    case NS_SOCK_WRITE:
        do {
           n = SSL_write(sesPtr->ssl, sesPtr->next, sesPtr->len);
        } while (n == -1 && SSL_get_error(sesPtr->ssl, n) == SSL_ERROR_SYSCALL && errno == EINTR);

        if (n < 0) {
            sesPtr->error = "send failed";
        } else {
            sesPtr->next += n;
            sesPtr->len -= n;
            if (sesPtr->len == 0) {
                shutdown(sock, 1);
                Tcl_DStringTrunc(&sesPtr->ds, 0);
                Ns_TaskCallback(task, NS_SOCK_READ, &sesPtr->timeout);
            }
            return;
        }
        break;

    case NS_SOCK_READ:
        do {
           n = SSL_read(sesPtr->ssl, buf, sizeof(buf));
        } while (n == -1 && SSL_get_error(sesPtr->ssl, n) == SSL_ERROR_SYSCALL && errno == EINTR);

        if (n > 0) {
            Tcl_DStringAppend(&sesPtr->ds, buf, n);
            return;
        }
        if (n < 0) {
            sesPtr->error = "recv failed";
        }
        break;

    case NS_SOCK_DONE:
        return;

    case NS_SOCK_TIMEOUT:
        sesPtr->error = "timeout";
        break;

    case NS_SOCK_EXIT:
        sesPtr->error = "shutdown";
        break;

    case NS_SOCK_CANCEL:
        sesPtr->error = "cancelled";
        break;
    }

    /*
     * Get completion time and mark task as done.
     */

    Ns_GetTime(&sesPtr->etime);
    Ns_TaskDone(sesPtr->task);
}

/*
 *----------------------------------------------------------------------
 *
 * SetDeferAccept --
 *
 *      Tell the OS not to give us a new socket until data is available.
 *      This saves overhead in the poll() loop and the latency of a RT.
 *
 *      Otherwise, we will get socket as soon as the TCP connection
 *      is established.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Disabled by default as Linux seems broken (does not respect
 *      the timeout, linux-2.6.26).
 *
 *----------------------------------------------------------------------
 */

static void
SetDeferAccept(Ns_Driver *driver, NS_SOCKET sock)
{
    SSLDriver *cfg = driver->arg;

    if (cfg->deferaccept) {
#ifdef TCP_DEFER_ACCEPT
        int sec;

        sec = driver->recvwait;
        if (setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT,
                       &sec, sizeof(sec)) == -1) {
            Ns_Log(Error, "nssock: setsockopt(TCP_DEFER_ACCEPT): %s",
                   ns_sockstrerror(ns_sockerrno));
        }
#else
# ifdef SO_ACCEPTFILTER
        struct accept_filter_arg afa;
	int n;

	memset(&afa, 0, sizeof(afa));
	strcpy(afa.af_name, "httpready");
	n = setsockopt(sock, SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa));
        if (n < 0) {
	    Ns_Log(Error, "nssock: setsockopt(SO_ACCEPTFILTER): %s",
                   ns_sockstrerror(ns_sockerrno));
	}
# endif
#endif
    }
}
