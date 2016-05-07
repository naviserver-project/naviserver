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
 * connchan.c --
 *
 *      Support functions for connection channels
 */

#include "nsd.h"


/*
 * Structure handling one registered channel for the [ns_connchan] command
 */

typedef struct {
    const char      *channelName;
    char             peer[NS_IPADDR_SIZE];  /* Client peer address */
    size_t           rBytes;
    size_t           wBytes;
    bool             binary;
    Ns_Time          startTime;
    Sock            *sockPtr;
    Ns_Time          recvTimeout;
    Ns_Time          sendTimeout;
    const char      *clientData;
    struct Callback *cbPtr;
} NsConnChan;


typedef struct Callback {
    NsConnChan  *connChanPtr;
    const char  *threadName;
    unsigned int when;
    size_t       scriptLength;
    char         script[1];
} Callback;


/*
 * Local functions defined in this file
 */

static void CallbackFree(Callback *cbPtr)
    NS_GNUC_NONNULL(1);

static NsConnChan *ConnChanCreate(NsServer *servPtr, Sock *sockPtr,
                                  Ns_Time *startTime, const char *peer, bool binary, 
                                  const char *clientData) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4)
    NS_GNUC_RETURNS_NONNULL;

static void ConnChanFree(NsConnChan *connChanPtr) 
    NS_GNUC_NONNULL(1);

static NsConnChan *ConnChanGet(Tcl_Interp *interp, NsServer *servPtr, const char *name)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int SockCallbackRegister(NsConnChan *connChanPtr, const char *script, unsigned int when, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static ssize_t DriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static ssize_t DriverSend(Sock *sockPtr, const struct iovec *bufs, int nbufs, unsigned int flags, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

static Ns_SockProc NsTclConnChanProc;


/*
 *----------------------------------------------------------------------
 *
 * CallbackFree --
 *
 *    Free Callback structure and unregister socket callback
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Freeing memory.
 *
 *----------------------------------------------------------------------
 */
static void
CallbackFree(Callback *cbPtr)
{
    NS_NONNULL_ASSERT(cbPtr != NULL);
    NS_NONNULL_ASSERT(cbPtr->connChanPtr != NULL);
    
    Ns_SockCancelCallbackEx(cbPtr->connChanPtr->sockPtr->sock, NULL, NULL, NULL);
    cbPtr->connChanPtr->cbPtr = NULL;
    
    ns_free(cbPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanCreate --
 *
 *    Allocate a connecion channel strucuture and initialize its fields.
 *
 * Results:
 *    Initialized connection channel structure.
 *
 * Side effects:
 *    Allocating memory.
 *
 *----------------------------------------------------------------------
 */
static NsConnChan *
ConnChanCreate(NsServer *servPtr, Sock *sockPtr, Ns_Time *startTime, const char *peer, bool binary, const char *clientData) {
    static uintptr_t  connchanCount = 0;
    NsConnChan       *connChanPtr;
    Tcl_HashEntry    *hPtr;
    char              name[5 + TCL_INTEGER_SPACE];
    int               isNew;
    
    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(startTime != NULL);
    NS_NONNULL_ASSERT(peer != NULL);

    /*
     * Lock the channel table and create a new entry for the
     * connection.
     */

    Ns_MutexLock(&servPtr->connchans.lock);
    snprintf(name, sizeof(name), "conn%td", connchanCount ++);
    hPtr = Tcl_CreateHashEntry(&servPtr->connchans.table, name, &isNew);
    Ns_MutexUnlock(&servPtr->connchans.lock);

    if (likely(isNew == 0)) {
        Ns_Log(Warning, "duplicate connchan name '%s'", name);
    }
   
    connChanPtr = ns_malloc(sizeof(NsConnChan));
    Tcl_SetHashValue(hPtr, connChanPtr);
        
    connChanPtr->channelName = ns_strdup(name);
    connChanPtr->cbPtr = NULL;
    connChanPtr->startTime = *startTime;
    connChanPtr->rBytes = 0;
    connChanPtr->wBytes = 0;
    connChanPtr->recvTimeout.sec = 0;
    connChanPtr->recvTimeout.usec = 0;
    connChanPtr->sendTimeout.sec = 0;
    connChanPtr->sendTimeout.usec = 0;
    connChanPtr->clientData = clientData != NULL ? ns_strdup(clientData) : NULL;

    strncpy(connChanPtr->peer, peer, NS_IPADDR_SIZE);
    connChanPtr->sockPtr = sockPtr;
    connChanPtr->binary = binary;

    return connChanPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanFree --
 *
 *    Free NsConnChan structure and remove the entry from the hash table of
 *    open connection channel structures.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Freeing memory
 *
 *----------------------------------------------------------------------
 */
static void
ConnChanFree(NsConnChan *connChanPtr) {
    NsServer *servPtr;
    Tcl_HashEntry  *hPtr;

    NS_NONNULL_ASSERT(connChanPtr != NULL);
    assert(connChanPtr->sockPtr != NULL);
    assert(connChanPtr->sockPtr->servPtr != NULL);

    servPtr = connChanPtr->sockPtr->servPtr;
    /*
     * Remove entry from hash table.
     */
    Ns_MutexLock(&servPtr->connchans.lock);
    hPtr = Tcl_FindHashEntry(&servPtr->connchans.table, connChanPtr->channelName);
    if (hPtr != NULL) {
        Tcl_DeleteHashEntry(hPtr);
    } else {
        Ns_Log(Error, "ns_connchan: could not delete hash entry for channel '%s'\n",
               connChanPtr->channelName);
    }
            
    Ns_MutexUnlock(&servPtr->connchans.lock);

    /*
     * Free connChanPtr content.
     */
    if (connChanPtr->cbPtr != NULL) {
        CallbackFree((Callback *)connChanPtr->cbPtr);
    }
    ns_free((char *)connChanPtr->channelName);
    if (connChanPtr->clientData != NULL) {
        ns_free((char *)connChanPtr->clientData);
    }

    NsSockClose(connChanPtr->sockPtr, NS_FALSE);
    ns_free((char *)connChanPtr);

}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanGet --
 *
 *    Access a NsConnChan from the per-server table via its name
 *
 * Results:
 *    ConnChan* or NULL if not found.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
static NsConnChan *
ConnChanGet(Tcl_Interp *interp, NsServer *servPtr, const char *name) {
    Tcl_HashEntry  *hPtr;
    NsConnChan     *connChanPtr = NULL;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    
    Ns_MutexLock(&servPtr->connchans.lock);
    hPtr = Tcl_FindHashEntry(&servPtr->connchans.table, name);
    if (hPtr != NULL) {
        connChanPtr = (NsConnChan *)Tcl_GetHashValue(hPtr);
    }
    Ns_MutexUnlock(&servPtr->connchans.lock);

    if (connChanPtr == NULL && interp != NULL) {
        Ns_TclPrintfResult(interp, "connchan \"%s\" does not exist", name);
    }

    return connChanPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockProc --
 *
 *      A wrapper function callback that is called, when the callback is
 *      fired. The function allocates an interpreter if necessary, builds the
 *      argument list for invocation and calls the registered Tcl script.
 *
 * Results:
 *      NS_TRUE or NS_FALSE on error 
 *
 * Side effects:
 *      Will run Tcl script. 
 *
 *----------------------------------------------------------------------
 */

static bool
NsTclConnChanProc(NS_SOCKET sock, void *arg, unsigned int why)
{
    Tcl_DString  script;
    Callback    *cbPtr;
    Tcl_Interp  *interp;
    const char  *w;
    int          result;

    NS_NONNULL_ASSERT(arg != NULL);

    cbPtr = arg;

    Ns_Log(Ns_LogConnchanDebug, "NsTclConnChanProc why %u", why);
    
    assert(cbPtr->connChanPtr != NULL);
    assert(cbPtr->connChanPtr->sockPtr != NULL);
    assert(cbPtr->connChanPtr->sockPtr->servPtr != NULL);
    
    if (why == (unsigned int)NS_SOCK_EXIT) {
    fail:
        if (cbPtr->connChanPtr != NULL) {
            ConnChanFree(cbPtr->connChanPtr);
        }
        return NS_FALSE;
    }

    Tcl_DStringInit(&script);
    Tcl_DStringAppend(&script, cbPtr->script, cbPtr->scriptLength);
    
    if ((why & (unsigned int)NS_SOCK_TIMEOUT) != 0u) {
        w = "t";
    } else if ((why & (unsigned int)NS_SOCK_READ) != 0u) {
        w = "r";
    } else if ((why & (unsigned int)NS_SOCK_WRITE) != 0u) {
        w = "w";
    } else if ((why & (unsigned int)NS_SOCK_EXCEPTION) != 0u) {
        w = "e";
    } else {
        w = "x";
    }
        
    Tcl_DStringAppendElement(&script, w);

    interp = NsTclAllocateInterp(cbPtr->connChanPtr->sockPtr->servPtr);
    result = Tcl_EvalEx(interp, script.string, script.length, 0);
    
    if (result != TCL_OK) {
        (void) Ns_TclLogErrorInfo(interp, "\n(context: connchan proc)");
    } else {
        Tcl_Obj *objPtr = Tcl_GetObjResult(interp);
        int      ok = 1;

        Ns_Log(Ns_LogConnchanDebug, "NsTclConnChanProc: Tcl eval returned <%s>", Tcl_GetString(objPtr));
        result = Tcl_GetBooleanFromObj(interp, objPtr, &ok);
        if (result == TCL_OK && ok == 0) {
            result = TCL_ERROR;
        }
    }
    Ns_TclDeAllocateInterp(interp);
    Tcl_DStringFree(&script);
    
    if (result != TCL_OK) {
        goto fail;
    }

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SockCallbackRegister --
 *
 *      Register a callback for the connection channel. Due to the underlying
 *      infrastructure, one socket has at most one callback registered.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      Memory management for the callback strucuture.
 *
 *----------------------------------------------------------------------
 */

static int
SockCallbackRegister(NsConnChan *connChanPtr, const char *script, unsigned int when, const Ns_Time *timeoutPtr)
{
    Callback *cbPtr;
    size_t    scriptLength;
    int       result;

    NS_NONNULL_ASSERT(connChanPtr != NULL);
    NS_NONNULL_ASSERT(script != NULL);

    /*
     * If there is already a callback registered, free and cancel it. This
     * has to be done as first step, since CallbackFree() calls finally
     * Ns_SockCancelCallbackEx(), which deletes all callbacks registered for
     * the associated socket.
     */
    if (connChanPtr->cbPtr != NULL) {
        CallbackFree((Callback *)connChanPtr->cbPtr);
    }
    
    scriptLength = strlen(script);

    cbPtr = ns_malloc(sizeof(Callback) + (size_t)scriptLength);
    memcpy(cbPtr->script, script, scriptLength + 1u);
    cbPtr->scriptLength = scriptLength;
    cbPtr->when = when;
    cbPtr->threadName = NULL;
    cbPtr->connChanPtr = connChanPtr;
    
    result = Ns_SockCallbackEx(connChanPtr->sockPtr->sock, NsTclConnChanProc, cbPtr,
                               when | (unsigned int)NS_SOCK_EXIT, 
                               timeoutPtr, &cbPtr->threadName);
    if (result == TCL_OK) {
        assert(connChanPtr->cbPtr == NULL);
        connChanPtr->cbPtr = cbPtr;
    } else {
        CallbackFree(cbPtr);
    } 
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DriverRecv --
 *
 *      Read data from the socket into the given vector of buffers.
 *
 * Results:
 *      Number of bytes read, or -1 on error.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
DriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs, Ns_Time *timeoutPtr)
{
    Ns_Time timeout;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);
    NS_NONNULL_ASSERT(timeoutPtr != NULL);

    if (timeoutPtr->sec == 0 && timeoutPtr->usec == 0) {
        /*
         * Use configured receivewait as timeout.
         */
        timeout.sec = sockPtr->drvPtr->recvwait;
        timeoutPtr = &timeout;
    }

    return (*sockPtr->drvPtr->recvProc)((Ns_Sock *) sockPtr, bufs, nbufs, timeoutPtr, 0u);
}

/*
 *----------------------------------------------------------------------
 *
 * DriverSend --
 *
 *      Write a vector of buffers to the socket via the driver callback.
 *
 * Results:
 *      Number of bytes written, or -1 on error.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
DriverSend(Sock *sockPtr, const struct iovec *bufs, int nbufs, unsigned int flags, const Ns_Time *timeoutPtr)
{
    Ns_Time timeout;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    assert(sockPtr->drvPtr != NULL);

    if (timeoutPtr->sec == 0 && timeoutPtr->usec == 0) {
        /*
         * Use configured sendwait as timeout,
         */
        timeout.sec = sockPtr->drvPtr->sendwait;
        timeoutPtr = &timeout;
    }

    return (*sockPtr->drvPtr->sendProc)((Ns_Sock *) sockPtr, bufs, nbufs,
                                        timeoutPtr, flags);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConnChanObjCmd --
 *
 *    Implement the ns_connchan command.
 *
 * Results:
 *    Tcl result. 
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */

int
NsTclConnChanObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp       *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result = TCL_OK, opt;
    const char     *name = NULL;
    Tcl_HashEntry  *hPtr;
    NsConnChan     *connChanPtr;

    static const char *const opts[] = {
        "close",
        "callback",
        "detach",
        "exists",
        "list", 
        "open",
        "read",
        "write",  NULL
    };

    enum {
        CCloseIdx,
        CCallbackIdx,
        CDetachIdx,
        CExistsIdx,
        CListIdx, 
        COpenIdx,
        CReadIdx,
        CWriteIdx 
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, 
                            "option", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }
    
    switch (opt) {
    case CDetachIdx:
        {
            /*
             * ns_connchan detach
             */
            Conn       *connPtr = (Conn *)itPtr->conn;
            NsConnChan *connChanPtr;
            
            if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }
            if (connPtr == NULL) {
                Ns_TclPrintfResult(interp, "no current connection");
                return TCL_ERROR;
            }

            /*
             * Lock the channel table and create a new entry for the
             * connection. After this operation the channel is responsible for
             * managing the sockPtr, so we have to remove it from the
             * connection structure.
             */
            connChanPtr = ConnChanCreate(servPtr,
                                         connPtr->sockPtr,
                                         Ns_ConnStartTime((Ns_Conn *)connPtr),
                                         connPtr->reqPtr->peer,
                                         (connPtr->flags & NS_CONN_WRITE_ENCODED) != 0u ? NS_FALSE : NS_TRUE,
                                         connPtr->clientData);
            connPtr->sockPtr = NULL;
            Tcl_SetObjResult(interp, Tcl_NewStringObj(connChanPtr->channelName, -1));

            break;
        }

    case COpenIdx:
        {
            /*
             * ns_connchan open
             */
            const char  *url;
            Sock        *sockPtr;
            Ns_Set      *hdrPtr = NULL;
            char        *method = "GET";
            Ns_Time      timeout = {1, 0}, *timeoutPtr = &timeout; 
            Ns_ObjvSpec  lopts[] = {
                {"-headers", Ns_ObjvSet,    &hdrPtr, NULL},
                {"-method",  Ns_ObjvString, &method, NULL},
                {"-timeout",  Ns_ObjvTime,  &timeoutPtr,  NULL},
                {NULL, NULL, NULL, NULL}
            };
            Ns_ObjvSpec  largs[] = {
                {"url", Ns_ObjvString, &url, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(lopts, largs, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }
            
            result = NSDriverClientOpen(interp, url, method, timeoutPtr, &sockPtr);
            if (likely(result == TCL_OK)) {
                Ns_Time      now;
                NsConnChan  *connChanPtr;
                struct iovec buf[4];
                ssize_t      nSent;

                if (STREQ(sockPtr->drvPtr->protocol, "https")) {
                    Ns_ClientConnectionContext ccc;

                    assert(sockPtr->drvPtr->clientInitProc != NULL);

                    /* 
                     * For the time being, just pass NULL
                     * structures. Probably, we could create the SSLcontext
                     * here and pass it on to reduce the number of members,
                     * ... when we link against OpenSSL similar as against
                     * zlib (with stubs in case it is not available).
                     */
                    ccc.ctx.https.cert = NULL;
                    ccc.ctx.https.caFile = NULL;
                    ccc.ctx.https.caPath = NULL;
                    ccc.ctx.https.verify = 0;

                    result = (*sockPtr->drvPtr->clientInitProc)(interp, (Ns_Sock *)sockPtr, &ccc);
                    
                    if (unlikely(result != TCL_OK)) {
                        if (sockPtr->sock > 0) {ns_sockclose(sockPtr->sock);}
                        return TCL_ERROR;
                    }
                }
                
                Ns_GetTime(&now);
                connChanPtr = ConnChanCreate(servPtr,
                                             sockPtr,
                                             &now,
                                             sockPtr->reqPtr->peer,
                                             NS_TRUE /* binary, fixed for the time being */,
                                             NULL);
                if (hdrPtr != NULL) {
                    int i;
                    
                    for (i = 0u; i < Ns_SetSize(hdrPtr); i++) {
                        const char *key = Ns_SetKey(hdrPtr, i);
                        Ns_DStringPrintf(&sockPtr->reqPtr->buffer, "%s: %s\r\n", key, Ns_SetValue(hdrPtr, i));
                    }
                }
                
                /*
                 * Write the request header via the "send" operation of the driver.
                 */
                buf[0].iov_base = (void *)sockPtr->reqPtr->request.line;
                buf[0].iov_len = strlen(buf[0].iov_base);
                buf[1].iov_base = (void *)"\r\n";
                buf[1].iov_len = 2u;
                buf[2].iov_base = (void *)sockPtr->reqPtr->buffer.string;
                buf[2].iov_len = Tcl_DStringLength(&sockPtr->reqPtr->buffer);
                buf[3].iov_base = (void *)"\r\n";
                buf[3].iov_len = 2u;
                
                nSent = DriverSend(connChanPtr->sockPtr, buf, 4, 0u, &connChanPtr->sendTimeout);
                Ns_Log(Ns_LogConnchanDebug, "DriverSend sent %ld bytes <%s>", nSent, strerror(errno));

                if (nSent > -1) {
                    connChanPtr->wBytes += (size_t)nSent;
                    //Tcl_SetObjResult(interp, Tcl_NewLongObj((long)nSent));
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(connChanPtr->channelName, -1));
                } else {
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(strerror(errno), -1));
                    result = TCL_ERROR;
                }
            }
            break;
        }
        
    case CListIdx:
        {
            /*
             * ns_connchan list
             */
            Tcl_HashSearch  search;
            const char     *server = NULL;
            Tcl_DString     ds, *dsPtr = &ds;
            
            Ns_ObjvSpec lopts[] = {
                {"-server", Ns_ObjvString, &server, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }
            if (server != NULL) {
                servPtr = NsGetServer(server);
                if (servPtr == NULL) {
                    Ns_TclPrintfResult(interp, "server \"%s\" does not exist", server);
                    return TCL_ERROR;
                }
            }

            /*
             * The provided parameter appear to be valid. Lock the channel
             * table and return the infos for every existing entry in the
             * conneciton channel table.
             */
            Tcl_DStringInit(dsPtr);

            Ns_MutexLock(&servPtr->connchans.lock);
            hPtr = Tcl_FirstHashEntry(&servPtr->connchans.table, &search);
            while (hPtr != NULL) {
                connChanPtr = (NsConnChan *)Tcl_GetHashValue(hPtr);
                Ns_DStringPrintf(dsPtr, "{%s %s %" PRIu64 ".%06ld %s %s %" PRIdz " %" PRIdz,
                                 Tcl_GetHashKey(&servPtr->connchans.table, hPtr),
                                 (connChanPtr->cbPtr != NULL && connChanPtr->cbPtr->threadName != NULL) ?
                                 connChanPtr->cbPtr->threadName : "{}",
                                 (int64_t) connChanPtr->startTime.sec, connChanPtr->startTime.usec,
                                 connChanPtr->sockPtr->drvPtr->name,
                                 connChanPtr->peer,
                                 connChanPtr->wBytes,
                                 connChanPtr->rBytes);
                Ns_DStringAppendElement(dsPtr,
                                        (connChanPtr->clientData != NULL) ? connChanPtr->clientData : "");
                Ns_DStringAppend(dsPtr, "} ");
                hPtr = Tcl_NextHashEntry(&search);
            }
            Ns_MutexUnlock(&servPtr->connchans.lock);

            Tcl_DStringResult(interp, dsPtr);
            break;
        }
        
    case CCloseIdx:
        {
            /*
             * ns_connchan close
             */
            Ns_ObjvSpec args[] = {
                {"channel", Ns_ObjvString, &name, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }


            connChanPtr = ConnChanGet(interp, servPtr, name);
            Ns_Log(Notice, "ns_connchan %s close connChanPtr %p", name, (void*)connChanPtr);

            if (connChanPtr != NULL) {
                ConnChanFree(connChanPtr);
            } else {
                result = TCL_ERROR;
            }

            break;
        }

    case CCallbackIdx:
        {
            /*
             * ns_connchan callback
             */
            const char *script, *whenString;
            Ns_Time     *pollTimeoutPtr = NULL, *recvTimeoutPtr = NULL, *sendTimeoutPtr = NULL;
            
            Ns_ObjvSpec lopts[] = {
                {"-timeout",    Ns_ObjvTime, &pollTimeoutPtr, NULL},
                {"-receivetimeout", Ns_ObjvTime, &recvTimeoutPtr, NULL},
                {"-sendtimeout",    Ns_ObjvTime, &sendTimeoutPtr, NULL},
                {NULL, NULL, NULL, NULL}
            };
            Ns_ObjvSpec args[] = {
                {"channel", Ns_ObjvString, &name, NULL},
                {"script", Ns_ObjvString, &script, NULL},
                {"when", Ns_ObjvString, &whenString, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }

            connChanPtr = ConnChanGet(interp, servPtr, name);
            if (likely(connChanPtr != NULL)) {
                /*
                 * The provided channel name exists. In a first step get the
                 * flags from the when string.
                 */
                unsigned int  when = 0u;
                const char   *s = whenString;
                
                while (*s != '\0') {
                    if (*s == 'r') {
                        when |= (unsigned int)NS_SOCK_READ;
                    } else if (*s == 'w') {
                        when |= (unsigned int)NS_SOCK_WRITE;
                    } else if (*s == 'e') {
                        when |= (unsigned int)NS_SOCK_EXCEPTION;
                    } else if (*s == 'x') {
                        when |= (unsigned int)NS_SOCK_EXIT;
                    } else {
                        Ns_TclPrintfResult(interp, "invalid when specification: \"%s\":"
                                           " should be one/more of r, w, e, or x", whenString);
                        return TCL_ERROR;
                    }
                    s++;
                }

                /*
                 * Fill in the timeouts, when these are provided.
                 */
                if (recvTimeoutPtr != NULL) {
                    connChanPtr->recvTimeout = *recvTimeoutPtr;
                }
                if (sendTimeoutPtr != NULL) {
                    connChanPtr->sendTimeout = *sendTimeoutPtr;
                }

                /*
                 * Register the callback.
                 */
                result = SockCallbackRegister(connChanPtr, script, when, pollTimeoutPtr);
                if (result != TCL_OK) {
                    Tcl_SetResult(interp, "could not register callback", TCL_STATIC);
                    ConnChanFree(connChanPtr);
                } else {
                    /*
                     * The socket is already in non-blocking state, since it
                     * was received via the driver.
                     */
                }
            } else {
                result = TCL_ERROR;
            }
            break;
        }

    case CExistsIdx:
        {
            /*
             * ns_connchan exists
             */
            Ns_ObjvSpec args[] = {
                {"channel", Ns_ObjvString, &name, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }

            connChanPtr = ConnChanGet(interp, servPtr, name);
            
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(connChanPtr != NULL));
            break;
        }
        

    case CReadIdx:
        {
            /*
             * ns_connchan read
             */
            Ns_ObjvSpec  args[] = {
                {"channel", Ns_ObjvString, &name, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }

            connChanPtr = ConnChanGet(interp, servPtr, name);
            if (likely(connChanPtr != NULL)) {
                /*
                 * The provided channel exists.
                 */
                ssize_t      nRead;
                struct iovec buf;
                char         buffer[4096];

                if (connChanPtr->binary != NS_TRUE) {
                    Ns_Log(Warning, "ns_connchan: only binary channels are currently supported. "
                           "Channel %s is not binary", name);
                }

                /*
                 * Read the data via the "receive" operation of the driver.
                 */
                buf.iov_base = buffer;
                buf.iov_len = sizeof(buffer);
                nRead = DriverRecv(connChanPtr->sockPtr, &buf, 1, &connChanPtr->recvTimeout);

                if (nRead > -1) {
                    connChanPtr->rBytes += (size_t)nRead;
                    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char *)buffer, (int)nRead));
                } else {
                    /*
                     * The receive operation failed, maybe a receive timeout
                     * happend.  The read call will simply return an empty
                     * string. We could notice this fact internally by a
                     * timeout counter, but for the time being no application
                     * has usage for it.
                     */
                }
            } else {
                result = TCL_ERROR;
            }

            break;
        }
        
    case CWriteIdx:
        {
            /*
             * ns_connchan write
             */
            Tcl_Obj     *msgObj;
            Ns_ObjvSpec  args[] = {
                {"channel", Ns_ObjvString, &name, NULL},
                {"msg",  Ns_ObjvObj,    &msgObj, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }

            connChanPtr = ConnChanGet(interp, servPtr, name);
            if (likely(connChanPtr != NULL)) {
                /*
                 * The provided channel name exists.
                 */
                struct iovec buf;
                ssize_t      nSent;
                int          msgLen;
                const char  *msgString = (const char *)Tcl_GetByteArrayFromObj(msgObj, &msgLen);

                if (connChanPtr->binary != NS_TRUE) {
                    Ns_Log(Warning, "ns_connchan: only binary channels are currently supported. "
                           "Channel %s is not binary", name);
                }

                /*
                 * Write the data via the "send" operation of the driver.
                 */
                buf.iov_base = (void *)msgString;
                buf.iov_len = (size_t)msgLen;
                nSent = DriverSend(connChanPtr->sockPtr, &buf, 1, 0u, &connChanPtr->sendTimeout);

                if (nSent > -1) {
                    connChanPtr->wBytes += (size_t)nSent;
                    Tcl_SetObjResult(interp, Tcl_NewLongObj((long)nSent));
                } else {
                    result = TCL_ERROR;
                }
            } else {
                result = TCL_ERROR;
            }

            break;
        }

    default:
        /* 
         * unexpected value 
         */
        assert(opt && 0);
        break;
    }

    return result;
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
