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

#define NSSSL_VERSION  "0.3"

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

static int SSLInterpInit(Tcl_Interp *interp, void *arg);
static int SSLObjCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);
static int SSLPassword(char *buf, int num, int rwflag, void *userdata);
static void SSLLock(int mode, int n, const char *file, int line);
static unsigned long SSLThreadId(void);
static int HttpsConnect(Tcl_Interp *interp, char *method, char *url, Ns_Set *hdrPtr,
			Tcl_Obj *bodyPtr, char *cert, char *caFile, char *caPath, int verify,
			Https **httpsPtrPtr);
static void HttpsClose(Https *httpsPtr);
static void HttpsCancel(Https *httpsPtr);
static void HttpsAbort(Https *httpsPtr);
static Https *HttpsGet(Tcl_Interp *interp, char *id);
static Ns_TaskProc HttpsProc;

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
     * Https cache support
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
	    SSL_set_mode(sslPtr->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
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
		if (decork) {Ns_SockCork(sock, 0);}
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

    if (decork) {Ns_SockCork(sock, 0);}
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

        Ns_ObjvSpec opts[] = {
            {"-timeout",  Ns_ObjvTime,    &timeoutPtr,  NULL},
            {"-headers",  Ns_ObjvSet,     &hdrPtr,      NULL},
            {"-method",   Ns_ObjvString,  &method,      NULL},
            {"-cert",     Ns_ObjvString,  &cert,        NULL},
            {"-cafile",   Ns_ObjvString,  &caFile,      NULL},
            {"-capath",   Ns_ObjvString,  &caPath,      NULL},
            {"-body",     Ns_ObjvObj,     &bodyPtr,     NULL},
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
	if (HttpsConnect(interp, method, url, hdrPtr, bodyPtr, 
			 cert, caFile, caPath, verify,
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
	int spoolLimit = -1;

        Ns_ObjvSpec opts[] = {
            {"-timeout",   Ns_ObjvTime, &timeoutPtr,     NULL},
            {"-headers",   Ns_ObjvSet,  &hdrPtr,         NULL},
            {"-elapsed",   Ns_ObjvObj,  &elapsedVarPtr,  NULL},
            {"-result",    Ns_ObjvObj,  &resultVarPtr,   NULL},
            {"-status",    Ns_ObjvObj,  &statusVarPtr,   NULL},
	    {"-file",      Ns_ObjvObj,  &fileVarPtr,     NULL},
	    {"-spoolsize", Ns_ObjvInt,  &spoolLimit,     NULL},
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

	Ns_Log(Notice, "SSL request finished %d <%s>", httpPtr->status, httpPtr->replyHeaders->name);
	if (httpPtr->status == 0) {
	    Ns_Log(Notice, "======= SSL response <%s>", httpPtr->ds.string);
	}

        if (statusVarPtr != NULL && !Ns_SetNamedVar(interp, statusVarPtr, Tcl_NewIntObj(httpPtr->status))) {
            HttpsClose(httpsPtr);
            return TCL_ERROR;
        }
	
	if (httpPtr->spoolFd > 0)  {
	    close(httpPtr->spoolFd);
	    valPtr = Tcl_NewObj();
	} else {
	    valPtr = Tcl_NewByteArrayObj((unsigned char*)httpPtr->ds.string + httpPtr->replyHeaderSize, 
					 (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
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
	     char *cert, char *caFile, char *caPath, int verify,
	     Https **httpsPtrPtr)
{
    NS_SOCKET    sock;
    Ns_HttpTask *httpPtr = NULL;
    Https       *httpsPtr = NULL;
    int          len, portNr, uaFlag = -1;
    char        *host, *file, *port, *body;
    char         hostBuffer[256];
    
    /*
     * Parse and split url
     */
    
    if (strncmp(url, "https://", 8) != 0 || url[8] == '\0') {
	Tcl_AppendResult(interp, "invalid url: ", url, NULL);
	return TCL_ERROR;
    }
    host = url + 8;
    file = strchr(host, '/');
    // Ns_Log(Notice, "XXX search host <%s> for slash => file <%s>", host, file);
    if (file != NULL) {
	*file = '\0';
    }
    //Ns_Log(Notice, "XXX remaining host <%s>", host);
    port = strchr(host, ':');
    if (port == NULL) {
	portNr = 443;
    } else {
	*port = '\0';
	portNr = (int) strtol(port+1, NULL, 10);
    }

    //Ns_Log(Notice, "XXX url <%s> port %d host <%s> file <%s>", url, portNr, host, file);
    strncpy(hostBuffer, host, sizeof(hostBuffer));
    
    /*
     * Connect to the host and allocate session struct
     */

    sock = Ns_SockAsyncConnect(hostBuffer, portNr);
    if (sock == INVALID_SOCKET) {
	Tcl_AppendResult(interp, "connect to ", url, " failed: ", ns_sockstrerror(ns_sockerrno), NULL);
	return TCL_ERROR;
    }

    if (file != NULL) {
	*file = '/';
    }

    httpsPtr = ns_calloc(1, sizeof(Https));
    httpPtr  = &httpsPtr->http;

    httpPtr->sock            = sock;
    httpPtr->spoolLimit      = -1;
    httpPtr->url             = ns_strdup(url);
    Ns_MutexInit(&httpPtr->lock);
    /*Ns_MutexSetName(&httpPtr->lock, name, buffer);*/
    Tcl_DStringInit(&httpPtr->ds);

    //Ns_Log(Notice, "url <%s> port %d sock %d host <%s> file <%s>", httpPtr->url, portNr, sock, hostBuffer, file);

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
    
    Ns_DStringPrintf(&httpPtr->ds, "%s %s HTTP/1.0\r\n", method, file ? file : "/");

    /*
     * Submit provided headers
     */
    
    if (hdrPtr != NULL) {
	int i;

	/*
	 * Remove the header fields, we are providing
	 */
	Ns_SetIDeleteKey(hdrPtr, "Host");
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
    
    if (port == NULL) {
	Ns_DStringPrintf(&httpPtr->ds, "Host: %s\r\n", hostBuffer);
    } else {
	Ns_DStringPrintf(&httpPtr->ds, "Host: %s:%d\r\n", hostBuffer, portNr);
    }

    body = NULL;
    if (bodyPtr != NULL) {
	body = Tcl_GetStringFromObj(bodyPtr, &len);
	if (len == 0) {
	    body = NULL;
	}
    }
    if (body != NULL) {
	Ns_DStringPrintf(&httpPtr->ds, "Content-Length: %d\r\n", len);
    }
    Tcl_DStringAppend(&httpPtr->ds, "\r\n", 2);
    if (body != NULL) {
	Tcl_DStringAppend(&httpPtr->ds, body, len);
    }
    httpPtr->next = httpPtr->ds.string;
    httpPtr->len = httpPtr->ds.length;

    /*Ns_Log(Notice, "final request <%s>", httpPtr->ds.string);*/
    
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
    Ns_MutexDestroy(&httpPtr->lock);
    Tcl_DStringFree(&httpPtr->ds);
    ns_free(httpPtr->url);
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
HttpsProc(Ns_Task *task, SOCKET sock, void *arg, int why)
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
		/*Ns_Log(Notice, "SSL WRITE done, switch to READ");*/
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

	/*Ns_Log(Notice, "Task READ got %d bytes err %d", (int)n, err);*/
	
        if (likely(n > 0)) {
	    /* 
	     * In case we are spooling, write to the spoolfile,
	     * otherwise append to the DString. Spooling is only
	     * activated after (a) having processed the headers, and
	     * (b) after the wait command has required to spool. Both
	     * conditions are necessary, but might be happen in
	     * different orders.
	     */
	    if (httpPtr->spoolFd > 0) {
		Ns_Log(Debug, "Task got %d bytes, spooled", (int)n);
		write(httpPtr->spoolFd, buf, n);
	    } else {
		Tcl_DStringAppend(&httpPtr->ds, buf, n);
		if (unlikely(httpPtr->replyHeaderSize == 0)) {
		    Ns_HttpCheckHeader(httpPtr);
		}
		/*
		 * Ns_HttpCheckSpool might set httpPtr->spoolFd
		 */
		Ns_HttpCheckSpool(httpPtr);
		/*Ns_Log(Notice, "Task got %d bytes, header = %d", (int)n, httpPtr->replyHeaderSize);*/
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
    }

    /*
     * Get completion time and mark task as done.
     */

    Ns_GetTime(&httpPtr->etime);
    Ns_TaskDone(httpPtr->task);
}

