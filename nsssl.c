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
 * Copyright (C) 2001-2006 Vlad Seryakov
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

#define SSL_VERSION  "0.1"

typedef struct {
    SSL_CTX   *ctx;
    Ns_Mutex  lock;
} SSLDriver;

static Ns_DriverProc SSLProc;
static int SSLInterpInit(Tcl_Interp *interp, void *arg);
static int SSLCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);
static int SSLPassword(char *buf, int num, int rwflag, void *userdata);
static void SSLLock(int mode, int n, const char *file, int line);
static unsigned long SSLThreadId(void);

NS_EXPORT int Ns_ModuleVersion = 1;

static Ns_Mutex *locks;

NS_EXPORT int Ns_ModuleInit(char *server, char *module)
{
    Ns_DString ds;
    int num, n;
    char *path, *value;
    SSLDriver *drvPtr;
    Ns_DriverInitData init;

    drvPtr = ns_calloc(1, sizeof(SSLDriver));

    init.version = NS_DRIVER_VERSION_1;
    init.name = "nsssl";
    init.proc = SSLProc;
    init.opts = NS_DRIVER_SSL|NS_DRIVER_QUEUE_ONACCEPT;
    init.arg = drvPtr;
    init.path = NULL;

    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "nsssl: driver init failed.");
        ns_free(drvPtr);
        return NS_ERROR;
    }
    path = Ns_ConfigGetPath(server,module,NULL);

    Ns_DStringInit(&ds);

    num = CRYPTO_num_locks();
    locks = ns_calloc(num, sizeof(*locks));
    for (n = 0; n < num; n++) {
	Ns_DStringPrintf(&ds, "nsssl:%d", n);
        Ns_MutexSetName(locks + n, ds.string);
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

    // Load certificate and private key
    value = Ns_ConfigGetValue(path, "certificate");
    if (value == NULL) {
        Ns_Log(Error, "nsssl: certificate parameter should be specified");
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

    // Session cache support
    Ns_DStringPrintf(&ds, "nsssl:%d", getpid());
    SSL_CTX_set_session_id_context(drvPtr->ctx, (void *) ds.string, ds.length);
    SSL_CTX_set_session_cache_mode(drvPtr->ctx, SSL_SESS_CACHE_SERVER);

    // Parse SSL protocols
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

    // Parse SSL ciphers
    value = Ns_ConfigGetValue(path, "ciphers");
    if (value != NULL && SSL_CTX_set_cipher_list(drvPtr->ctx, value) == 0) {
        Ns_Log(Error, "nsssl: error loading ciphers: %s", value);
    }

    SSL_CTX_set_default_passwd_cb(drvPtr->ctx, SSLPassword);
    SSL_CTX_set_mode(drvPtr->ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_SINGLE_DH_USE);
    SSL_CTX_set_options(drvPtr->ctx, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

    // Seed the OpenSSL Pseudo-Random Number Generator.
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

    Ns_TclRegisterTrace(server, SSLInterpInit, drvPtr, NS_TCL_TRACE_CREATE);
    Ns_DStringFree(&ds);
    Ns_Log(Notice, "nsssl: version %s loaded", SSL_VERSION);
    return NS_OK;
}

static int SSLInterpInit(Tcl_Interp *interp, void *arg)
{
    Tcl_CreateObjCommand(interp, "ns_ssl", SSLCmd, arg, NULL);
    return NS_OK;
}

static int SSLCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[])
{
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SSLProc --
 *
 *	Driver proc for SSL requests
 *
 * Results:
 *	NS_OK or NS_ERROR
 *
 * Side effects:
 *  	None
 *
 *----------------------------------------------------------------------
 */

static int SSLProc(Ns_DriverCmd cmd, Ns_Sock *sock, struct iovec *bufs, int nbufs)
{
    SSLDriver *drvPtr = sock->driver->arg;
    SSL *sslPtr = sock->arg;
    int rc, size;

    switch(cmd) {
     case DriverQueue:
         if (sslPtr == NULL) {
             sslPtr = SSL_new(drvPtr->ctx);
             if (sslPtr == NULL) {
                 Ns_Log(Error, "%d: SSL session init error [%s]", sock->sock, strerror(errno));
                 return NS_FATAL;
             }
             SSL_set_fd(sslPtr, sock->sock);
             SSL_set_accept_state(sslPtr);
             sock->arg = sslPtr;
         }
         return NS_OK;

     case DriverRecv:
         while (1) {
             ERR_clear_error();
             rc = SSL_read(sslPtr, bufs->iov_base, bufs->iov_len);
             if (rc < 0 && SSL_get_error(sslPtr, rc) == SSL_ERROR_WANT_READ) {
                 Ns_Time timeout = { sock->driver->recvwait, 0 };
                 if (Ns_SockTimedWait(sock->sock, NS_SOCK_READ, &timeout) == NS_OK) {
                     continue;
                 }
                 SSL_set_shutdown(sslPtr, SSL_RECEIVED_SHUTDOWN);
             }
             break;
         }
         return rc;

     case DriverSend:
         size = 0;
         while (nbufs > 0) {
             ERR_clear_error();
             rc = SSL_write(sslPtr, bufs->iov_base, bufs->iov_len);
             if (rc < 0) {
                 if (SSL_get_error(sslPtr, rc) == SSL_ERROR_WANT_WRITE) {
                     Ns_Time timeout = { sock->driver->sendwait, 0 };
                     if (Ns_SockTimedWait(sock->sock, NS_SOCK_WRITE, &timeout) == NS_OK) {
                         continue;
                     }
                 }
                 SSL_set_shutdown(sslPtr, SSL_RECEIVED_SHUTDOWN);
                 return -1;
             }
             nbufs--;
             bufs++;
             size += rc;
         }
         return size;

     case DriverKeep:
         if (SSL_get_shutdown(sslPtr) == 0) {
             BIO *bio = SSL_get_wbio(sslPtr);
             if (bio != NULL && BIO_flush(bio) == 1) {
                 return NS_OK;
             }
         }
         break;

     case DriverClose:
         if (sslPtr != NULL) {
             for (rc = 0; rc < 4 && !SSL_shutdown(sslPtr); rc++);
             SSL_free(sslPtr);
         }
         sock->arg = NULL;
         return NS_OK;
    }
    return NS_ERROR;
}

static int SSLPassword(char *buf, int num, int rwflag, void *userdata)
{
    fprintf(stdout, "Enter SSL password:");
    fgets(buf, num, stdin);
    return(strlen(buf));
}

static void SSLLock(int mode, int n, const char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        Ns_MutexLock(locks + n);
    } else {
        Ns_MutexUnlock(locks + n);
    }
}

static unsigned long SSLThreadId(void)
{
    return (unsigned long) Ns_ThreadId();
}
