/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 */

/*
 * connchan.c --
 *
 *      Support functions for connection channels.
 */

#include "nsd.h"

#ifdef HAVE_OPENSSL_EVP_H
# include "nsopenssl.h"
# include <openssl/rand.h>
#endif

/*
 * Handling of network byte order (big endian).
 */
#if defined(__linux__)
# include <endian.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__)
# include <sys/endian.h>
#elif defined(__OpenBSD__)
# include <sys/types.h>
# ifndef be16toh
#  define be16toh(x) betoh16(x)
#  define be32toh(x) betoh32(x)
#  define be64toh(x) betoh64(x)
# endif
#elif defined(__APPLE__) || defined(_WIN32)
# define be16toh(x) ntohs(x)
# define htobe16(x) htons(x)
# define be32toh(x) ntonl(x)
# define htobe32(x) htonl(x)
# if defined(_WIN32)
/*
 * Not sure, why htonll() and ntohll() are undefined in Visual Studio 2019:
 *
 *#  define be64toh(x) ntohll(x)
 *#  define htobe64(x) htonll(x)
 */
#  define htobe64(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#  define be64toh(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
# else
#  define be64toh(x) ntohll(x)
#  define htobe64(x) htonll(x)
# endif
#endif

#define ConnChanBufferSize(connChanPtr, buf) ((connChanPtr)->buf != NULL ? (connChanPtr)->buf->length : 0)
#define ConnChanBufferAddress(connChanPtr, buf) (void*)((connChanPtr)->buf != NULL ? (connChanPtr)->buf->string : 0)

typedef struct Callback {
    NsConnChan  *connChanPtr;
    const char  *threadName;
    unsigned int when;
    size_t       scriptLength;
    size_t       scriptCmdNameLength;
    char         script[1];
} Callback;

/*
 * The following structure is used for a socket listen callback.
 */

typedef struct ListenCallback {
    const char *server;
    const char *driverName;
    char  script[1];
} ListenCallback;


/*
 * Local functions defined in this file.
 */
static Ns_ArgProc ArgProc;

static void CancelCallback(const NsConnChan *connChanPtr)
    NS_GNUC_NONNULL(1);

static NsConnChan *ConnChanCreate(NsServer *servPtr, Sock *sockPtr,
                                  const Ns_Time *startTime, const char *peer, bool binary,
                                  const char *clientData)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_RETURNS_NONNULL;

static void ConnChanFree(NsConnChan *connChanPtr, NsServer *servPtr)
    NS_GNUC_NONNULL(1);

static ssize_t ConnChanReadBuffer(NsConnChan *connChanPtr, char *buffer, size_t bufferSize)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static NsConnChan *ConnChanGet(Tcl_Interp *interp, NsServer *servPtr, const char *name)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static Ns_ReturnCode SockCallbackRegister(NsConnChan *connChanPtr, Tcl_Obj *scriptObj,
                                          unsigned int when, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static ssize_t ConnchanDriverSend(Tcl_Interp *interp, const NsConnChan *connChanPtr,
                                  struct iovec *bufs, int nbufs, unsigned int flags,
                                  const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(6);

static TCL_SIZE_T CompactBuffers(NsConnChan *connChanPtr, const char *msgString, TCL_SIZE_T msgLength, ssize_t bytesSent,
                                 struct iovec *iovecs, int nBuffers, size_t toSend, int caseInt)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

static void CompactSendBuffer(NsConnChan  *connChanPtr, struct iovec *iovecPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static size_t PrepareSendBuffers(NsConnChan *connChanPtr, const char *msgString, TCL_SIZE_T msgLength,
                                 struct iovec *iovecs, int *nBuffers, int *caseInt)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)  NS_GNUC_NONNULL(5);

static void DebugLogBufferState(NsConnChan *connChanPtr, size_t bytesToSend, ssize_t bytesSent, const char *data, const char *fmt, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(5)
    NS_GNUC_PRINTF(5, 6);

static char *WhenToString(char *buffer, unsigned int when)
    NS_GNUC_NONNULL(1);

static bool SockListenCallback(NS_SOCKET sock, void *arg, unsigned int UNUSED(why));
static void RequireDsBuffer(Tcl_DString **dsPtr)  NS_GNUC_NONNULL(1);
static void WebsocketFrameSetCommonMembers(Tcl_Obj *resultObj, ssize_t nRead, const NsConnChan *connChanPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static Ns_SockProc NsTclConnChanProc;

static TCL_OBJCMDPROC_T   ConnChanCallbackObjCmd;
static TCL_OBJCMDPROC_T   ConnChanCloseObjCmd;
static TCL_OBJCMDPROC_T   ConnChanDetachObjCmd;
static TCL_OBJCMDPROC_T   ConnChanDebugObjCmd;
static TCL_OBJCMDPROC_T   ConnChanExistsObjCmd;
static TCL_OBJCMDPROC_T   ConnChanListObjCmd;
static TCL_OBJCMDPROC_T   ConnChanListenObjCmd;
static TCL_OBJCMDPROC_T   ConnChanOpenObjCmd;
static TCL_OBJCMDPROC_T   ConnChanReadObjCmd;
static TCL_OBJCMDPROC_T   ConnChanWriteObjCmd;
static TCL_OBJCMDPROC_T   ConnChanWsencodeObjCmd;

static Ns_SockProc CallbackFree;


/*
 *----------------------------------------------------------------------
 *
 * WhenToString --
 *
 *      Converts socket condition flags to a human-readable string.
 *      The provided input buffer must be at least 5 bytes long.
 *
 * Results:
 *      A pointer to the resulting null-terminated string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static char*
WhenToString(char *buffer, unsigned int when) {
    char *p = buffer;

    NS_NONNULL_ASSERT(buffer != NULL);

    if ((when & (unsigned int)NS_SOCK_READ) != 0u) {
        *p++ = 'r';
    }
    if ((when & (unsigned int)NS_SOCK_WRITE) != 0u) {
        *p++ = 'w';
    }
    if ((when & (unsigned int)NS_SOCK_EXCEPTION) != 0u) {
        *p++ = 'e';
    }
    if ((when & (unsigned int)NS_SOCK_EXIT) != 0u) {
        *p++ = 'x';
    }
    *p = '\0';

    return buffer;
}


/*
 *----------------------------------------------------------------------
 *
 * CallbackFree --
 *
 *      Frees a Callback structure and unregisters the associated socket callback.
 *
 * Results:
 *      Returns NS_TRUE if the callback was freed successfully, otherwise NS_FALSE.
 *
 * Side effects:
 *      Frees memory and logs a warning if called with an unexpected reason.
 *
 *----------------------------------------------------------------------
 */

static bool
CallbackFree(NS_SOCKET UNUSED(sock), void *arg, unsigned int why) {
    bool result;

    if (why != (unsigned int)NS_SOCK_CANCEL) {
        Ns_Log(Warning, "connchan: CallbackFree called with unexpected reason code %u",
               why);
        result = NS_FALSE;

    } else {
        Callback *cbPtr = arg;

        Ns_Log(Ns_LogConnchanDebug, "connchan: CallbackFree cbPtr %p why %u",
               (void*)cbPtr, why);
        ns_free(cbPtr);
        result = NS_TRUE;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CancelCallback --
 *
 *      Cancels a socket callback and unregisters it from the socket.
 *      This function frees the associated callback structure by
 *      calling the underlying cancellation routine.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Unregisters and frees the callback structure.
 *
 *----------------------------------------------------------------------
 */

static void
CancelCallback(const NsConnChan *connChanPtr)
{
    NS_NONNULL_ASSERT(connChanPtr != NULL);
    NS_NONNULL_ASSERT(connChanPtr->cbPtr != NULL);

    Ns_Log(Ns_LogConnchanDebug, "%s connchan: CancelCallback %p",
           connChanPtr->channelName, (void*)connChanPtr->cbPtr);

    (void)Ns_SockCancelCallbackEx(connChanPtr->sockPtr->sock, CallbackFree,
                                  connChanPtr->cbPtr, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanCreate --
 *
 *      Allocates and initializes a new connection channel structure.
 *      If the provided peer is NULL, the function derives the peer
 *      address from the given socket.
 *
 * Results:
 *      A pointer to a fully initialized NsConnChan structure.
 *
 * Side effects:
 *      Allocates memory.
 *
 *----------------------------------------------------------------------
 */
static NsConnChan *
ConnChanCreate(NsServer *servPtr, Sock *sockPtr,
               const Ns_Time *startTime, const char *peer, bool binary,
               const char *clientData) {
    static uint64_t      connchanCount = 0u;
    NsConnChan          *connChanPtr;
    Tcl_HashEntry       *hPtr;
    char                 name[5 + TCL_INTEGER_SPACE];
    int                  isNew;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(startTime != NULL);

    Ns_SockSetKeepalive(sockPtr->sock, 1);

    /*
     * Create a new NsConnChan structure and fill in the elements,
     * which can be set without a lock. The intention is to keep the
     * lock-times low.
     */

    connChanPtr = ns_malloc(sizeof(NsConnChan));
    connChanPtr->cbPtr = NULL;
    connChanPtr->startTime = *startTime;
    connChanPtr->rBytes = 0;
    connChanPtr->wBytes = 0;
    connChanPtr->recvTimeout.sec = 0;
    connChanPtr->recvTimeout.usec = 0;
    connChanPtr->sendTimeout.sec = 0;
    connChanPtr->sendTimeout.usec = 0;
    connChanPtr->clientData = clientData != NULL ? ns_strdup(clientData) : NULL;
    connChanPtr->sendBuffer = NULL;
    connChanPtr->secondarySendBuffer = NULL;
    connChanPtr->frameBuffer = NULL;
    connChanPtr->fragmentsBuffer = NULL;
    connChanPtr->frameNeedsData = NS_TRUE;
    connChanPtr->debugLevel = 0;
    connChanPtr->debugFD = 0;
    connChanPtr->requireStableSendBuffer = STREQ(sockPtr->drvPtr->protocol, "https");

    if (peer == NULL) {
        (void)ns_inet_ntop((struct sockaddr *)&(sockPtr->sa), connChanPtr->peer, NS_IPADDR_SIZE);
    } else {
        strncpy(connChanPtr->peer, peer, NS_IPADDR_SIZE - 1);
    }
    connChanPtr->sockPtr = sockPtr;
    connChanPtr->binary = binary;
    memcpy(name, "conn", 4);

    /*
     * Lock the channel table and create a new entry in the hash table
     * for the connection. The counter-based name creation requires a
     * lock to guarantee unique names.
     */
    Ns_RWLockWrLock(&servPtr->connchans.lock);
    (void)ns_uint64toa(&name[4], connchanCount ++);
    hPtr = Tcl_CreateHashEntry(&servPtr->connchans.table, name, &isNew);

    if (unlikely(isNew == 0)) {
        Ns_Log(Warning, "duplicate connchan name '%s'", name);
    }

    Tcl_SetHashValue(hPtr, connChanPtr);

    connChanPtr->channelName = ns_strdup(name);
    Ns_RWLockUnlock(&servPtr->connchans.lock);

    return connChanPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanFree --
 *
 *      Frees a connection channel structure and removes its entry
 *      from the server's connection channel table.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees memory and unregisters the connection channel.
 *
 *----------------------------------------------------------------------
 */
static void
ConnChanFree(NsConnChan *connChanPtr, NsServer *servPtr) {
    Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(connChanPtr != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);

    //assert(connChanPtr->sockPtr != NULL);
    //assert(connChanPtr->sockPtr->servPtr != NULL);

    //servPtr = connChanPtr->sockPtr->servPtr;
    /*
     * Remove entry from hash table.
     */
    Ns_RWLockWrLock(&servPtr->connchans.lock);
    hPtr = Tcl_FindHashEntry(&servPtr->connchans.table, connChanPtr->channelName);
    if (hPtr != NULL) {
        Tcl_DeleteHashEntry(hPtr);
    } else {
        Ns_Log(Error, "ns_connchan: could not delete hash entry for channel '%s'",
               connChanPtr->channelName);
    }
    Ns_RWLockUnlock(&servPtr->connchans.lock);

    if (hPtr != NULL) {
        /*
         * Only in cases, where we found the entry, we can free the
         * connChanPtr content.
         */
        if (connChanPtr->cbPtr != NULL) {
            /*
             * Add CancelCallback() to the sock callback queue.
             */
            CancelCallback(connChanPtr);
            /*
             * There might be a race condition, when a previously
             * registered callback is currently active (or going to be
             * processed). So make sure, it won't access a stale
             * connChanPtr member.
             */
            connChanPtr->cbPtr->connChanPtr = NULL;
            /*
             * The cancel callback takes care about freeing the
             * actual callback.
             */
            connChanPtr->cbPtr = NULL;
        }
        ns_free((char *)connChanPtr->channelName);
        ns_free((char *)connChanPtr->clientData);

        if (connChanPtr->sockPtr != NULL) {
            NsSockClose(connChanPtr->sockPtr, (int)NS_FALSE);
            connChanPtr->sockPtr = NULL;
        }
        if (connChanPtr->sendBuffer != NULL) {
            Tcl_DStringFree(connChanPtr->sendBuffer);
            ns_free((char *)connChanPtr->sendBuffer);
        }
        if (connChanPtr->secondarySendBuffer != NULL) {
            Tcl_DStringFree(connChanPtr->secondarySendBuffer);
            ns_free((char *)connChanPtr->secondarySendBuffer);
        }
        if (connChanPtr->frameBuffer != NULL) {
            Tcl_DStringFree(connChanPtr->frameBuffer);
            ns_free((char *)connChanPtr->frameBuffer);
        }
        if (connChanPtr->fragmentsBuffer != NULL) {
            Tcl_DStringFree(connChanPtr->fragmentsBuffer);
            ns_free((char *)connChanPtr->fragmentsBuffer);
        }
        ns_free((char *)connChanPtr);
    } else {
        Ns_Log(Bug, "ns_connchan: could not delete hash entry for channel '%s'",
               connChanPtr->channelName);
    }

}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanGet, NsConnChanGet --
 *
 *      Retrieves a connection channel from the server's channel table
 *      by name.
 *
 * Results:
 *      Pointer to NsConnChan if found, otherwise NULL. If the channel is not
 *      found and an interpreter is provided, an error message is set in the
 *      interpreter.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static NsConnChan *
ConnChanGet(Tcl_Interp *interp, NsServer *servPtr, const char *name) {
    const Tcl_HashEntry *hPtr;
    NsConnChan          *connChanPtr;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    Ns_RWLockRdLock(&servPtr->connchans.lock);
    hPtr = Tcl_FindHashEntry(&servPtr->connchans.table, name);
    connChanPtr = hPtr != NULL ? (NsConnChan *)Tcl_GetHashValue(hPtr) : NULL;
    Ns_RWLockUnlock(&servPtr->connchans.lock);

    if (connChanPtr == NULL && interp != NULL) {
        Ns_TclPrintfResult(interp, "channel \"%s\" does not exist", name);
    }

    return connChanPtr;
}

NsConnChan *NsConnChanGet(Tcl_Interp *interp, NsServer *servPtr, const char *name)
{
    return ConnChanGet(interp, servPtr, name);

}

/*
 *----------------------------------------------------------------------
 *
 * NsConnChanGetSendErrno --
 *
 *      Retrieves the send error code from the socket of a connection channel.
 *
 * Results:
 *      A generalized error code (which may include POSIX and OpenSSL errors).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
unsigned long
NsConnChanGetSendErrno(Tcl_Interp *UNUSED(interp), NsServer *servPtr, const char *name)
{
    const Tcl_HashEntry *hPtr;
    unsigned long        result = 0;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    Ns_RWLockRdLock(&servPtr->connchans.lock);
    hPtr = Tcl_FindHashEntry(&servPtr->connchans.table, name);
    if (hPtr != NULL) {
        NsConnChan *connChanPtr = (NsConnChan *)Tcl_GetHashValue(hPtr);
        result = (connChanPtr != NULL && connChanPtr->sockPtr != NULL)
            ? connChanPtr->sockPtr->sendErrno
            : 0u;
    }
    Ns_RWLockUnlock(&servPtr->connchans.lock);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConnChanProc --
 *
 *      A callback wrapper function for socket events. When the
 *      registered socket callback is fired, this function allocates
 *      an interpreter (if needed), builds the argument list, and
 *      calls the registered Tcl script.
 *
 * Results:
 *      Returns NS_TRUE if the callback was processed successfully; otherwise,
 *      NS_FALSE.
 *
 * Side effects:
 *      May invoke a Tcl script and free the connection channel if the
 *      callback signals to close the channel.
 *
 *----------------------------------------------------------------------
 */

static bool
NsTclConnChanProc(NS_SOCKET UNUSED(sock), void *arg, unsigned int why)
{
    Callback *cbPtr;
    bool      success = NS_TRUE;

    NS_NONNULL_ASSERT(arg != NULL);

    cbPtr = arg;

    if (cbPtr->connChanPtr == NULL) {
        /*
         * Safety belt.
         */
        Ns_Log(Ns_LogConnchanDebug, "NsTclConnChanProc called on a probably deleted callback %p",
               (void*)cbPtr);
        success = NS_FALSE;

    } else {
        char      whenBuffer[6] = {0};
        NsServer *servPtr;

        /*
         * We should have a valid callback structure that we test
         * with asserts (cbPtr->connChanPtr != NULL):
         */
        Ns_Log(Ns_LogConnchanDebug, "%s NsTclConnChanProc why %s (%u)",
               cbPtr->connChanPtr->channelName, WhenToString(whenBuffer, why), why);

        assert(cbPtr->connChanPtr->sockPtr != NULL);
        servPtr = cbPtr->connChanPtr->sockPtr->servPtr;

        if (why == (unsigned int)NS_SOCK_EXIT) {
            /*
             * Treat the "exit" case like error cases and free in such
             * cases the connChanPtr structure.
             */
            success = NS_FALSE;

        } else {
            int             result;
            Tcl_DString     script;
            Tcl_Interp     *interp;
            const char     *w, *channelName;
            bool            logEnabled;
            size_t          scriptCmdNameLength;
            NS_SOCKET       localsock;

            /*
             * In all remaining cases, the Tcl callback is executed.
             */
            assert(servPtr != NULL);

            Tcl_DStringInit(&script);
            Tcl_DStringAppend(&script, cbPtr->script, (TCL_SIZE_T)cbPtr->scriptLength);

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

            if (Ns_LogSeverityEnabled(Ns_LogConnchanDebug)) {
                logEnabled = NS_TRUE;
                channelName = ns_strdup(cbPtr->connChanPtr->channelName);
                scriptCmdNameLength = cbPtr->scriptCmdNameLength;
            } else {
                logEnabled = NS_FALSE;
                channelName = NULL;
                scriptCmdNameLength = 0u;
            }
            Tcl_DStringAppendElement(&script, w);

            localsock = cbPtr->connChanPtr->sockPtr->sock;
            interp = NsTclAllocateInterp(servPtr);
            result = Tcl_EvalEx(interp, script.string, script.length, 0);

            if (result != TCL_OK) {
                (void) Ns_TclLogErrorInfo(interp, "\n(context: connchan proc)");
            } else {
                Tcl_DString ds;
                Tcl_Obj    *objPtr = Tcl_GetObjResult(interp);
                int         ok = 1;

                /*
                 * Here we cannot trust the cbPtr structure anymore,
                 * since the call to Tcl_EvalEx might have deleted it
                 * via some script.
                 */

                if (logEnabled) {
                    Tcl_DStringInit(&ds);
                    Tcl_DStringAppend(&ds, script.string, (TCL_SIZE_T)scriptCmdNameLength);
                    Ns_Log(Ns_LogConnchanDebug,
                           "%s NsTclConnChanProc Tcl eval <%s> returned <%s>",
                           channelName, ds.string, Tcl_GetString(objPtr));
                    Tcl_DStringFree(&ds);
                }

                /*
                 * The Tcl callback can signal with the result "0",
                 * that the connection channel should be closed
                 * automatically. A result of "2" means to suspend
                 * (cancel) the callback, but not close the channel.
                 */
                result = Tcl_GetIntFromObj(interp, objPtr, &ok);
                if (result == TCL_OK) {
                    Ns_Log(Ns_LogConnchanDebug, "NsTclConnChanProc <%s> numeric result %d", script.string, ok);
                    if (ok == 0) {
                        result = TCL_ERROR;
                    } else if (ok == 2) {
                        if (logEnabled) {
                            Ns_Log(Ns_LogConnchanDebug, "%s NsTclConnChanProc client "
                                   "requested to CANCEL (suspend) callback %p",
                                   channelName, (void*)cbPtr);
                        }
                        /*
                         * Use the "raw" Ns_SockCancelCallbackEx() API
                         * call to just stop socket handling, while
                         * keeping the connchan specific structures
                         * alive (postponing cleanup to a "close"
                         * operation).
                         *
                         * We cannot pass "callbackFree" and "cbPtr"
                         * to Ns_SockCancelCallbackEx(), so the
                         * callback structure will stay around until
                         * the callback is reset or the channel is
                         * finally cleaned. We might consider changing
                         * the condition flags (also from the
                         * scripting level) to turn callbacks for
                         * certain conditions on or off.
                         */
                        (void) Ns_SockCancelCallbackEx(localsock, NULL, NULL, NULL);

                    }
                } else {
                    Tcl_DStringInit(&ds);
                    Tcl_DStringAppend(&ds, script.string, (TCL_SIZE_T)scriptCmdNameLength);

                    Ns_Log(Warning, "%s callback <%s> returned unhandled result '%s' (must be 0, 1, or 2)",
                           channelName,
                           ds.string,
                           Tcl_GetString(objPtr));
                    Tcl_DStringFree(&ds);
                }
            }
            ns_free((char *)channelName);

            Ns_TclDeAllocateInterp(interp);
            Tcl_DStringFree(&script);

            if (result != TCL_OK) {
                success = NS_FALSE;
            }
        }

        if (!success) {
            if (cbPtr->connChanPtr != NULL) {
                Ns_Log(Ns_LogConnchanDebug, "%s NsTclConnChanProc free channel",
                       cbPtr->connChanPtr->channelName);
                ConnChanFree(cbPtr->connChanPtr, servPtr);
                cbPtr->connChanPtr = NULL;
            }
        }
    }
    return success;
}




/*
 *----------------------------------------------------------------------
 *
 * ArgProc --
 *
 *      Appends callback information for logging purposes.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies the provided Tcl_DString to include callback details.
 *
 *----------------------------------------------------------------------
 */

static void
ArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const Callback *cbPtr = arg;

    Tcl_DStringStartSublist(dsPtr);
    if (cbPtr->connChanPtr != NULL) {
        /*
         * It might be the case that the connChanPtr was canceled, but
         * the updatecmd not yet executed.
         */
        Tcl_DStringAppend(dsPtr, cbPtr->connChanPtr->channelName, TCL_INDEX_NONE);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Tcl_DStringAppend(dsPtr, cbPtr->script, (TCL_SIZE_T)cbPtr->scriptCmdNameLength);
    } else {
        Ns_Log(Notice, "connchan ArgProc cbPtr %p has no connChanPtr", (void*)cbPtr);
    }
    Tcl_DStringEndSublist(dsPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * SockCallbackRegister --
 *
 *      Registers a Tcl script callback for a connection channel.  If
 *      an existing callback is present, it is replaced with the new
 *      one.
 *
 * Results:
 *      Returns a standard NaviServer return code (NS_OK on success).
 *
 * Side effects:
 *      Allocates memory for the new callback structure and registers it
 *      with the underlying socket system.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
SockCallbackRegister(NsConnChan *connChanPtr, Tcl_Obj *scriptObj,
                     unsigned int when, const Ns_Time *timeoutPtr)
{
    Callback     *cbPtr;
    TCL_SIZE_T    scriptLength;
    Ns_ReturnCode result;
    const char   *p, *scriptString;

    NS_NONNULL_ASSERT(connChanPtr != NULL);
    NS_NONNULL_ASSERT(scriptObj != NULL);

    scriptString = Tcl_GetStringFromObj(scriptObj, &scriptLength);

    /*
     * If there is already a callback registered, free and cancel
     * it. This has to be done as first step, since CancelCallback()
     * calls finally Ns_SockCancelCallbackEx(), which deletes all
     * callbacks registered for the associated socket.
     */
    if (connChanPtr->cbPtr != NULL) {
        cbPtr = ns_realloc(connChanPtr->cbPtr, sizeof(Callback) + (size_t)scriptLength);

    } else {
        cbPtr = ns_malloc(sizeof(Callback) + (size_t)scriptLength);
    }
    memcpy(cbPtr->script, scriptString, (size_t)scriptLength + 1u);
    cbPtr->scriptLength = (size_t)scriptLength;

    /*
     * Keep the length of the cmd name for introspection and debugging
     * purposes. Rationale: The callback often contains binary data,
     * so outputting the full cmd mess up log files.
     *
     * Assumption: cmd name must not contain funny characters.
     */
    p = strchr(cbPtr->script, INTCHAR(' '));
    if (p != NULL) {
        cbPtr->scriptCmdNameLength = (size_t)(p - cbPtr->script);
    } else {
        cbPtr->scriptCmdNameLength = 0u;
    }
    cbPtr->when = when;
    cbPtr->threadName = NULL;
    cbPtr->connChanPtr = connChanPtr;

    result = Ns_SockCallbackEx(connChanPtr->sockPtr->sock, NsTclConnChanProc, cbPtr,
                               when | (unsigned int)NS_SOCK_EXIT,
                               timeoutPtr, &cbPtr->threadName);
    if (result == NS_OK) {
        connChanPtr->cbPtr = cbPtr;

        Ns_RegisterProcInfo((ns_funcptr_t)NsTclConnChanProc, "ns_connchan", ArgProc);
    } else {
        /*
         * The callback could not be registered, maybe the socket is
         * not valid anymore. Free the callback.
         */
        (void) CallbackFree(connChanPtr->sockPtr->sock, cbPtr, (unsigned int)NS_SOCK_CANCEL);
        connChanPtr->sockPtr = NULL;
        connChanPtr->cbPtr = NULL;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnchanDriverSend --
 *
 *      Sends a vector of data buffers over the socket associated with
 *      a connection channel.  Handles partial writes and timeouts. If
 *      a send operation is incomplete, the remaining data is either
 *      retried or buffered as needed.
 *
 * Results:
 *      Returns the total number of bytes successfully written, or -1 on error.
 *
 * Side effects:
 *      May adjust the state of the connection channel’s buffers and
 *      update send counters.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
ConnchanDriverSend(Tcl_Interp *interp, const NsConnChan *connChanPtr,
                   struct iovec *bufs, int nbufs, unsigned int flags,
                   const Ns_Time *timeoutPtr)
{
    ssize_t  result;
    Sock    *sockPtr;

    NS_NONNULL_ASSERT(connChanPtr != NULL);
    NS_NONNULL_ASSERT(timeoutPtr != NULL);

    sockPtr = connChanPtr->sockPtr;

    assert(sockPtr != NULL);
    assert(sockPtr->drvPtr != NULL);

    /*
     * Call the driver's sendProc, but handle also partial write
     * operations.
     *
     * In principle, we could call here simply
     *
     *     result = Ns_SockSendBufs((Ns_Sock *)sockPtr, bufs, nbufs, timeoutPtr, flags);
     *
     * but this operation can block the thread in cases where not all
     * of the buffer was sent. Therefore, the following block defines
     * a logic, where on partial operations, the remaining data is
     * passed to the caller, when no send timeout is specified.
     */
    if (likely(sockPtr->drvPtr->sendProc != NULL)) {
        bool    haveTimeout = NS_FALSE, partial;
        ssize_t bytesSent = 0, toSend = (ssize_t)Ns_SumVec(bufs, nbufs), origLength = toSend, partialResult;

        do {
            ssize_t       partialToSend = (ssize_t)Ns_SumVec(bufs, nbufs);
            char          errorBuffer[256];
            unsigned long sendErrno;

            Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend try to send [0] %" PRIdz
                   " bytes (total %"  PRIdz ")",
                   connChanPtr->channelName,
                   bufs->iov_len, partialToSend);

            partialResult = NsDriverSend(sockPtr, bufs, nbufs, flags);
            sendErrno = sockPtr->sendErrno;

            if (sendErrno != 0
                && sendErrno != ECONNRESET
                && !NsSockRetryCode((int)sendErrno)
                && (ssize_t)bufs->iov_len != partialResult
                ) {
                Ns_Log(Warning, "%s ConnchanDriverSend NsDriverSend tosend %" PRIdz " sent %"
                       PRIdz " errorState %.8lx --- %s",
                       connChanPtr->channelName, bufs->iov_len,
                       partialResult, sockPtr->sendErrno,
                       NsSockErrorCodeString(sockPtr->sendErrno, errorBuffer, sizeof(errorBuffer)));
            } else {
                Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend NsDriverSend sent %"
                       PRIdz " errorState %.8lx --- %s",
                       connChanPtr->channelName, partialResult, sockPtr->sendErrno,
                       NsSockErrorCodeString(sockPtr->sendErrno, errorBuffer, sizeof(errorBuffer)));
            }

            if (connChanPtr->debugLevel > 0) {
                Ns_Log(Notice, "NsDriverSend %s: toSend %ld sent %ld want_write %d total %ld",
                       connChanPtr->channelName, partialToSend, partialResult,
                       (sockPtr->flags & NS_CONN_SSL_WANT_WRITE) != 0u, bytesSent);
            }

            if (partialResult == 0) {
                /*
                 * The resource is temporarily unavailable, we can an
                 * retry, when the socket is writable.
                 */
                if (connChanPtr->requireStableSendBuffer && (sockPtr->flags & NS_CONN_SSL_WANT_WRITE) != 0u) {

                    ssize_t lastSendRejected = sockPtr->sendRejected;
                    if (lastSendRejected > 0 && lastSendRejected != partialToSend) {
                        Ns_Log(Notice, "%s ConnchanDriverSend sock (%d,%ld): reset sendRejected from %ld to %ld",
                               connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount, lastSendRejected, partialToSend);
                    } else if (lastSendRejected == 0 && partialToSend > 0) {
                        Ns_Log(Notice, "%s ConnchanDriverSend sock (%d,%ld): set sendRejected freshly to %ld",
                               connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount, partialToSend);
                    }
                    sockPtr->sendRejected = partialToSend;
                    sockPtr->sendRejectedBase = ConnChanBufferAddress(connChanPtr,sendBuffer);   // !!! can it be that send buffer address differs from iov_base?
                    //sockPtr->sendRejectedBase = bufs[0].iov_base;
                    Ns_Log(Notice, "REJECT HANDLING %s (%d,%ld): set sendRejectedBase to %p",
                           connChanPtr->channelName, sockPtr->sock, sockPtr->sendCount,
                           sockPtr->sendRejectedBase);
                }
                /*
                 * If there is no timeout provided, return the bytes sent so far.
                 */
                if (timeoutPtr->sec == 0 && timeoutPtr->usec == 0) {
                    Ns_Log(Ns_LogConnchanDebug,
                           "%s ConnchanDriverSend would block, no timeout configured, "
                           "origLength %" PRIdz" still to send %" PRIdz " already sent %" PRIdz,
                           connChanPtr->channelName, origLength, toSend, bytesSent);
                    break;

                } else {
                    /*
                     * A timeout was provided. Be aware that the timeout
                     * will suspend all sock-callback handlings for this
                     * time period in this thread.
                     */
                    Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend recoverable "
                           "error before timeout (" NS_TIME_FMT ")",
                           connChanPtr->channelName, (int64_t)timeoutPtr->sec, timeoutPtr->usec);

                    if (Ns_SockTimedWait(sockPtr->sock, (unsigned int)NS_SOCK_WRITE, timeoutPtr) == NS_OK) {
                        partialResult = NsDriverSend(sockPtr, bufs, nbufs, flags);
                    } else {
                        Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend timeout occurred",
                               connChanPtr->channelName);
                        haveTimeout = NS_TRUE;
                        Ns_TclPrintfResult(interp, "channel %s timeout on send "
                                           "operation (" NS_TIME_FMT ")",
                                           connChanPtr->channelName,
                                           (int64_t)timeoutPtr->sec, timeoutPtr->usec);
                        Tcl_SetErrorCode(interp, "NS_TIMEOUT", NS_SENTINEL);
                        Ns_Log(Ns_LogTimeoutDebug, "connchan send on %s runs into timeout",
                               connChanPtr->channelName);
                        partialResult = -1;
                    }
                }
            } else {
                if (sockPtr->sendRejected > 0) {
                    Ns_Log(Notice, "%s ConnchanDriverSend sock (%d,%ld): clear sendRejected, was %ld (we sent %ld)",
                           connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount, sockPtr->sendRejected, partialResult);
                    sockPtr->sendRejected = 0;
                    sockPtr->sendRejectedBase = 0;
                }
            }

            partial = NS_FALSE;

            if (partialResult != -1) {
                bytesSent += partialResult;
                partialToSend -= partialResult;

                Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend check partialResult %" PRIdz
                       " bytesSent %" PRIdz " toSend %" PRIdz " partial ? %d",
                       connChanPtr->channelName, partialResult, bytesSent, partialToSend, (partialToSend > 0));
                assert(partialToSend >= 0);

                if (partialToSend > 0) {
                    /*
                     * Partial write operation: part of the iovec has
                     * been sent, we have to retransmit the rest.
                     */
                    Ns_Log(Ns_LogConnchanDebug,
                           "%s ConnchanDriverSend partial write operation, sent %" PRIdz
                           " (so far %" PRIdz ") remaining %" PRIdz
                           " bytes, full length %" PRIdz,
                           connChanPtr->channelName, partialResult, bytesSent, partialToSend, origLength);
                    partial = NS_TRUE;
                }
                (void) Ns_ResetVec(bufs, nbufs, (size_t)partialResult);
                assert((size_t)partialToSend == Ns_SumVec(bufs, nbufs));

            } else if (!haveTimeout) {
                /*
                 * The "errno" variable might be 0 here (at least in
                 * https cases), when there are OpenSSL error cases
                 * not caused by the OS socket states. This can lead
                 * to weird looking exceptions, stating "Success"
                 * (errno == 0). Such errors happened e.g. before
                 * setting SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER. Hopefully,
                 * such error cases are eliminated already.  In case
                 * we see such errors again, we should look for a way
                 * to get the error message directly from the driver
                 * (e.g. from OpenSSL) and not from the OS.
                 */
                const char *errorMsg = Tcl_ErrnoMsg(errno);
                /*
                 * Timeout is handled above, all other errors ar
                 * handled here. Return these as POSIX errors.
                 */
                Ns_TclPrintfResult(interp, "channel %s send operation failed: %s",
                                   connChanPtr->channelName, errorMsg);
                Tcl_SetErrorCode(interp, "POSIX", Tcl_ErrnoId(), errorMsg, NS_SENTINEL);
            }

            Ns_Log(Ns_LogConnchanDebug, "%s ### check result %ld == -1 || %ld == %ld "
                   "(partial %d && ok %d) => try again %d",
                   connChanPtr->channelName,
                   partialResult, toSend, bytesSent,
                   partial, (partialResult != -1), (partial && (partialResult != -1)));

        } while (partial && (partialResult != -1));

        if (partialResult != -1) {
            result = bytesSent;
        } else {
            result = -1;
        }

    } else {
        Ns_TclPrintfResult(interp, "channel %s: no sendProc registered for driver %s",
                           connChanPtr->channelName, sockPtr->drvPtr->moduleName);
        result = -1;
    }
    if (connChanPtr->debugLevel > 0) {
        Ns_Log(Notice, "NsDriverSend returns %ld", result);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanDetachObjCmd --
 *
 *      Implements the Tcl command "ns_connchan detach" to detach a
 *      connection channel from the current connection. This operation
 *      transfers control of the underlying socket from the main
 *      connection to the detached channel, allowing the detached
 *      channel to manage its own socket operations independently.
 *      After detachment, the original connection will no longer use
 *      the socket, and its status will be marked as closed. The
 *      command returns the name of the detached channel for further
 *      reference.
 *
 * Results:

 *      Returns TCL_OK on success (with the detached channel name set
 *      as the interpreter’s result), or TCL_ERROR if the specified
 *      connection channel does not exist or if an error occurs during
 *      detachment.
 *
 * Side effects:
 *      - Removes the socket pointer from the current connection, effectively
 *        isolating the connection channel.
 *      - Marks the connection as closed, so that subsequent operations
 *        (such as sending a response) will not be attempted.
 *      - Frees associated resources by invoking ConnChanFree() on the
 *        detached channel.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanDetachObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    Conn           *connPtr = (Conn *)itPtr->conn;
    int             result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (connPtr == NULL) {
        Ns_TclPrintfResult(interp, "no current connection");
        result = TCL_ERROR;

    } else {
        NsServer         *servPtr = itPtr->servPtr;
        const NsConnChan *connChanPtr;

        /*
         * Lock the channel table and create a new entry for the
         * connection. After this operation the channel is responsible
         * for managing the sockPtr, so we have to remove it from the
         * connection structure.
         */
        connChanPtr = ConnChanCreate(servPtr,
                                     connPtr->sockPtr,
                                     Ns_ConnStartTime((Ns_Conn *)connPtr),
                                     Ns_ConnConfiguredPeerAddr(itPtr->conn),
                                     ((connPtr->flags & NS_CONN_WRITE_ENCODED) == 0u),
                                     connPtr->clientData);
        Ns_Log(Ns_LogConnchanDebug, "%s ConnChanDetachObjCmd sock %d",
               connChanPtr->channelName,
               connPtr->sockPtr->sock);

        connPtr->sockPtr = NULL;
        /*
         * All commands responding the client via this connection
         * can't work now, since response handling is delegated to the
         * connchan machinery. Make this situation detectable at the
         * script level via "ns_conn isconnected" by setting the close
         * flag.
         */
        connPtr->flags |= NS_CONN_CLOSED;

        Tcl_SetObjResult(interp, Tcl_NewStringObj(connChanPtr->channelName, TCL_INDEX_NONE));
        Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan detach returns %s",
               connChanPtr->channelName, Ns_TclReturnCodeString(result));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanOpenObjCmd --
 *
 *      Implements the "ns_connchan open" Tcl command, which
 *      establishes a new connection channel based on the provided
 *      host, port, and other optional parameters. This command
 *      initiates a socket connection (optionally over TLS), creates a
 *      connection channel, and returns the name of the channel for
 *      further operations.
 *
 *      The function parses optional arguments such as -cafile,
 *      -capath, -cert, -insecure, -hostname, -timeout, -tls, and
 *      -unix_socket to configure the TLS settings and connection
 *      behavior. If a TLS connection is requested and properly
 *      configured, the function creates an SSL context and
 *      initializes the connection via the driver's client
 *      initialization procedure.
 *
 *      Upon a successful connection, a new connection channel is
 *      allocated and added to the server's connection channel table,
 *      and the channel name is returned to the Tcl interpreter.
 *
 * Results:
 *      Returns TCL_OK if the connection channel is successfully
 *      created and initialized, setting the channel name as the Tcl
 *      result. Otherwise, returns TCL_ERROR along with an appropriate
 *      error message.
 *
 * Side effects:
 *      - Initiates a network connection (and TLS handshake, if applicable).
 *      - Allocates and registers a new connection channel.
 *      - May modify global server state if TLS contexts are created.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanOpenObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    NsInterp       *itPtr = clientData;
    int             result, insecureInt;
    Sock           *sockPtr = NULL;
    Ns_Set         *hdrPtr = NULL;
    char           *url, *method = (char *)"GET", *version = (char *)"1.0",
                   *driverName = NULL, *udsPath = NULL,
                   *sniHostname = NULL, *caFile = NULL, *caPath = NULL, *cert = NULL;
    Ns_Time         timeout = {1, 0}, *timeoutPtr = &timeout;
    Ns_ObjvSpec     lopts[] = {
        {"-cafile",      Ns_ObjvString, &caFile,      NULL},
        {"-capath",      Ns_ObjvString, &caPath,      NULL},
        {"-cert",        Ns_ObjvString, &cert,       NULL},
        {"-driver",      Ns_ObjvString, &driverName,  NULL},
        {"-headers",     Ns_ObjvSet,    &hdrPtr,      NULL},
        {"-hostname",    Ns_ObjvString, &sniHostname, NULL},
        {"-insecure",    Ns_ObjvBool,   &insecureInt, INT2PTR(NS_TRUE)},
        {"-method",      Ns_ObjvString, &method,      NULL},
        {"-timeout",     Ns_ObjvTime,   &timeoutPtr,  NULL},
        {"-unix_socket", Ns_ObjvString, &udsPath,     NULL},
        {"-version",     Ns_ObjvString, &version,     NULL},
        {"--",           Ns_ObjvBreak,  NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   largs[] = {
        {"url", Ns_ObjvString, &url, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);

    insecureInt = !itPtr->servPtr->httpclient.validateCertificates;

    if (Ns_ParseObjv(lopts, largs, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        NsServer    *servPtr = itPtr->servPtr;
        NsConnChan  *connChanPtr;
        Tcl_DString  ds;
        Ns_URL       parsedUrl;

        Tcl_DStringInit(&ds);
        result = NSDriverClientOpen(interp, driverName, url, method, version, udsPath,
                                    timeoutPtr, &ds,
                                    &parsedUrl, &sockPtr);
        if (likely(result == TCL_OK)) {

            if (STREQ(sockPtr->drvPtr->protocol, "https")) {
                NS_TLS_SSL_CTX *ctx;

                assert(sockPtr->drvPtr->clientInitProc != NULL);

                result = NsTlsGetParameters(itPtr, NS_TRUE, insecureInt,
                                            cert, caFile, caPath,
                                            (const char **)&caFile, (const char **)&caPath);
                if (result == TCL_OK) {
                    result = Ns_TLS_CtxClientCreate(interp,
                                                    cert, caFile,
                                                    caPath, insecureInt == 0,
                                                    &ctx);
                }

                if (likely(result == TCL_OK)) {
                    Ns_DriverClientInitArg params = {ctx, sniHostname, caFile, caPath};

                    if (sniHostname == NULL && !NsHostnameIsNumericIP(parsedUrl.host)) {
                        params.sniHostname = parsedUrl.host;
                        Ns_Log(Debug, "automatically use SNI <%s>", parsedUrl.host);
                    }
                    result = (*sockPtr->drvPtr->clientInitProc)(interp, (Ns_Sock *)sockPtr, &params);

                    /*
                     * For the time being, we create/delete the ctx in
                     * an eager fashion. We could probably make it
                     * reusable and keep it around.
                     */
                    if (ctx != NULL)  {
                        Ns_TLS_CtxFree(ctx);
                    }
                }
            }

            if (likely(result == TCL_OK)) {
                struct iovec buf[4];
                Ns_Time      now;
                ssize_t      nSent;

                Ns_GetTime(&now);
                connChanPtr = ConnChanCreate(servPtr,
                                             sockPtr,
                                             &now,
                                             NULL,
                                             NS_TRUE /* binary, fixed for the time being */,
                                             NULL);
                if (hdrPtr != NULL) {
                    size_t i;

                    for (i = 0u; i < Ns_SetSize(hdrPtr); i++) {
                        const char *key = Ns_SetKey(hdrPtr, i);
                        Ns_DStringPrintf(&sockPtr->reqPtr->buffer, "%s: %s\r\n", key, Ns_SetValue(hdrPtr, i));
                    }
                }

                Ns_Log(Ns_LogConnchanDebug, "ns_connchan open %s => %s",
                       url, connChanPtr->channelName);

                /*
                 * Write the request header via the "send" operation of
                 * the driver.
                 */
                buf[0].iov_base = (void *)sockPtr->reqPtr->request.line;
                buf[0].iov_len  = strlen(buf[0].iov_base);

                buf[1].iov_base = (void *)"\r\n";
                buf[1].iov_len  = 2u;

                buf[2].iov_base = (void *)sockPtr->reqPtr->buffer.string;
                buf[2].iov_len  = (size_t)Tcl_DStringLength(&sockPtr->reqPtr->buffer);

                buf[3].iov_base = (void *)"\r\n";
                buf[3].iov_len  = 2u;

                nSent = ConnchanDriverSend(interp, connChanPtr, buf, 4, 0u, &connChanPtr->sendTimeout);
                Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend sent %" PRIdz " bytes state %s",
                       connChanPtr->channelName,
                       nSent, errno != 0 ? strerror(errno) : "ok");

                if (nSent > -1) {
                    connChanPtr->wBytes += (size_t)nSent;
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(connChanPtr->channelName, TCL_INDEX_NONE));
                } else {
                    result = TCL_ERROR;
                }
            }
        }
        Tcl_DStringFree(&ds);

        if (unlikely(result != TCL_OK && sockPtr != NULL && sockPtr->sock > 0)) {
            ns_sockclose(sockPtr->sock);
        }
        Ns_Log(Ns_LogConnchanDebug, "ns_connchan open %s returns %s", url, Ns_TclReturnCodeString(result));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanConnectObjCmd --
 *
 *      Implements the "ns_connchan connect" Tcl command, which
 *      establishes a connection to a specified host and port,
 *      optionally initializing a TLS session if requested. The
 *      function parses the necessary connection parameters, such as
 *      host, port, and TLS-related options, and attempts to open a
 *      socket using a timed connection mechanism.
 *
 *      On a successful connection, the function creates a new
 *      connection channel, registers it in the server's connection
 *      channel table, and returns the channel name to the Tcl
 *      interpreter. In case of failure, it closes the socket and
 *      returns an error.
 *
 * Results:
 *      Returns TCL_OK on success, with the new connection channel
 *      name set as the Tcl interpreter's result. If the connection
 *      fails or parameters are invalid, returns TCL_ERROR and sets an
 *      appropriate error message.
 *
 * Side effects:
 *      - Initiates a socket connection (and TLS handshake if required).
 *      - May allocate memory for the new connection channel.
 *      - Updates the server's internal connection channel table.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanConnectObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    NsInterp       *itPtr = clientData;
    int             result, doTLS = (int)NS_FALSE, insecureInt;
    unsigned short  portNr = 0u;
    char           *host, *sniHostname = NULL, *caFile = NULL, *caPath = NULL, *cert = NULL;
    Ns_Time         timeout = {1, 0}, *timeoutPtr = &timeout;
    Ns_ObjvSpec     lopts[] = {
        {"-cafile",   Ns_ObjvString, &caFile,      NULL},
        {"-capath",   Ns_ObjvString, &caPath,      NULL},
        {"-cert",     Ns_ObjvString, &cert,        NULL},
        {"-hostname", Ns_ObjvString, &sniHostname, NULL},
        {"-insecure", Ns_ObjvBool,   &insecureInt, INT2PTR(NS_TRUE)},
        {"-timeout",  Ns_ObjvTime,   &timeoutPtr,  NULL},
        {"-tls",      Ns_ObjvBool,   &doTLS,       INT2PTR(NS_TRUE)},
        {"--",        Ns_ObjvBreak,  NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   largs[] = {
        {"host",    Ns_ObjvString, &host,   NULL},
        {"port",    Ns_ObjvUShort, &portNr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);

    insecureInt = !itPtr->servPtr->httpclient.validateCertificates;

    if (Ns_ParseObjv(lopts, largs, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (NsTlsGetParameters(itPtr, doTLS ==(int)NS_TRUE, insecureInt,
                                  cert, caFile, caPath,
                                  (const char **)&caFile, (const char **)&caPath) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        NsServer       *servPtr = itPtr->servPtr;
        Sock           *sockPtr = NULL;
        NS_SOCKET       sock;
        Ns_ReturnCode   status;

        sock = Ns_SockTimedConnect2(host, portNr, NULL, 0u, timeoutPtr, &status);

        if (sock == NS_INVALID_SOCKET) {
            Ns_SockConnectError(interp, host, portNr, status, timeoutPtr);
            result = TCL_ERROR;

        } else {
            result = NSDriverSockNew(interp, sock, doTLS ? "https" : "http", NULL, "CONNECT", &sockPtr);
        }

        if (likely(result == TCL_OK)) {

            if (STREQ(sockPtr->drvPtr->protocol, "https")) {
                NS_TLS_SSL_CTX *ctx;

                assert(sockPtr->drvPtr->clientInitProc != NULL);

                /*
                 * For the time being, just pass NULL
                 * structures. Probably, we could create the
                 * SSLcontext.
                 */
                result = Ns_TLS_CtxClientCreate(interp,
                                                NULL /*cert*/, NULL /*caFile*/,
                                                NULL /* caPath*/, NS_FALSE /*verify*/,
                                                &ctx);
                if (likely(result == TCL_OK)) {
                    Ns_DriverClientInitArg params = {ctx, host, caFile, caPath};

                    if (sniHostname == NULL && !NsHostnameIsNumericIP(host)) {
                        params.sniHostname = host;
                        Ns_Log(Debug, "automatically use SNI <%s>", host);
                    }

                    result = (*sockPtr->drvPtr->clientInitProc)(interp, (Ns_Sock *)sockPtr, &params);

                    /*
                     * For the time being, we create/delete the ctx in
                     * an eager fashion. We could probably make it
                     * reusable and keep it around.
                     */
                    if (ctx != NULL)  {
                        Ns_TLS_CtxFree(ctx);
                    }
                }
            }

            if (likely(result == TCL_OK)) {
                Ns_Time     now;
                NsConnChan *connChanPtr;

                Ns_GetTime(&now);
                connChanPtr = ConnChanCreate(servPtr,
                                             sockPtr,
                                             &now,
                                             NULL,
                                             NS_TRUE /* binary, fixed for the time being */,
                                             NULL);

                Tcl_SetObjResult(interp, Tcl_NewStringObj(connChanPtr->channelName, TCL_INDEX_NONE));
            }

        }
        if (unlikely(result != TCL_OK && sockPtr != NULL && sockPtr->sock > 0)) {
            ns_sockclose(sockPtr->sock);
        }
        Ns_Log(Ns_LogConnchanDebug, "ns_connchan connect %s %hu returns %s",
               host, portNr, Ns_TclReturnCodeString(result));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanListenObjCmd --
 *
 *      Implements the Tcl command "ns_connchan listen", which
 *      configures the server to listen for incoming connections on a
 *      specified address and port.  This function registers a socket
 *      callback for new incoming connection requests, thereby
 *      allowing the server to accept new client connections
 *      asynchronously.
 *
 *      It parses command-line options for driver selection, server
 *      specification, binding options, and the callback script to be
 *      executed when a new connection is received.  Upon successful
 *      registration, it creates a listening socket and returns
 *      connection details (such as the listening socket's address,
 *      port, and channel name) as a Tcl list.
 *
 * Results:
*      A standard Tcl result code:
 *          TCL_OK    - if the listening socket is successfully created
 *                      and the callback is registered.
 *          TCL_ERROR - if argument parsing fails or the socket callback
 *                      registration fails.
 *      On success, the Tcl interpreter's result is set to a list containing
 *      the connection details.
 *
 * Side effects:
 *      - Allocates memory for a ListenCallback structure.
 *      - Registers a new socket callback via Ns_SockListenCallback.
 *      - May update the server's connection channel table.
 *      - In case of failure, cleans up allocated resources.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanListenObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result, doBind = (int)NS_FALSE;
    unsigned short  port = 0u;
    char           *driverName = NULL, *addr = (char*)NS_EMPTY_STRING;
    Tcl_Obj        *scriptObj;
    Ns_ObjvSpec     lopts[] = {
        {"-driver",  Ns_ObjvString, &driverName, NULL},
        {"-server",  Ns_ObjvServer, &servPtr, NULL},
        {"-bind",    Ns_ObjvBool,   &doBind, INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak,  NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     largs[] = {
        {"address", Ns_ObjvString, &addr, NULL},
        {"port",    Ns_ObjvUShort, &port, NULL},
        {"script",  Ns_ObjvObj,    &scriptObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, largs, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        ListenCallback *lcbPtr;
        TCL_SIZE_T      scriptLength;
        char           *scriptString = Tcl_GetStringFromObj(scriptObj, &scriptLength);
        NS_SOCKET       sock;

        if (STREQ(addr, "*")) {
            addr = NULL;
        }
        lcbPtr = ns_malloc(sizeof(ListenCallback) + (size_t)scriptLength);
        if (unlikely(lcbPtr == NULL)) {
            return TCL_ERROR;
        }

        lcbPtr->server = servPtr->server;
        memcpy(lcbPtr->script, scriptString, (size_t)scriptLength + 1u);
        lcbPtr->driverName = ns_strcopy(driverName);
        sock = Ns_SockListenCallback(addr, port, SockListenCallback, doBind, lcbPtr);

        /* Ns_Log(Notice, "ns_connchan listen calls  Ns_SockListenCallback, returning %d", sock);*/
        if (sock == NS_INVALID_SOCKET) {
            Ns_TclPrintfResult(interp, "could not register callback");
            ns_free(lcbPtr);
            result = TCL_ERROR;
        } else {
            struct NS_SOCKADDR_STORAGE sa;
            socklen_t len = (socklen_t)sizeof(sa);
            Sock     *sockPtr = NULL;

            result = NSDriverSockNew(interp, sock, "http", lcbPtr->driverName, "CONNECT", &sockPtr);
            if (result == TCL_OK && sockPtr->servPtr != NULL) {
                Ns_Time     now;
                NsConnChan *connChanPtr;
                int         retVal;

                Ns_GetTime(&now);
                connChanPtr = ConnChanCreate(sockPtr->servPtr,
                                             sockPtr,
                                             &now,
                                             NULL,
                                             NS_TRUE /* binary, fixed for the time being */,
                                             NULL);
                retVal = getsockname(sock, (struct sockaddr *) &sa, &len);
                if (retVal == -1) {
                    Ns_TclPrintfResult(interp, "can't obtain socket info %s", ns_sockstrerror(ns_sockerrno));
                    ConnChanFree(connChanPtr, servPtr/*sockPtr->servPtr*/);
                    result = TCL_ERROR;
                } else {
                    Tcl_Obj  *listObj = Tcl_NewListObj(0, NULL);
                    char      ipString[NS_IPADDR_SIZE];

                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("channel", 7));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(connChanPtr->channelName, TCL_INDEX_NONE));

                    port = Ns_SockaddrGetPort((struct sockaddr *) &sa);
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("port", 4));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewIntObj((int)port));

                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("sock", 4));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewIntObj((int)sock));

                    ns_inet_ntop((struct sockaddr *) &sa, ipString, sizeof(ipString));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("address", 7));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ipString, TCL_INDEX_NONE));

                    Tcl_SetObjResult(interp, listObj);
                }
            }
        }
    }
    Ns_Log(Ns_LogConnchanDebug, "ns_connchan listen %s %hu returns %s", addr, port, Ns_TclReturnCodeString(result));
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SockListenCallback --
 *
 *      This is the C wrapper callback that is registered from
 *      ListenCallback.
 *
 * Results:
 *      NS_TRUE or NS_FALSE on error.
 *
 * Side effects:
 *      Will run Tcl script.
 *
 *----------------------------------------------------------------------
 */

static bool
SockListenCallback(NS_SOCKET sock, void *arg, unsigned int UNUSED(why))
{
    const ListenCallback *lcbPtr;
    Tcl_Interp           *interp;
    int                   result;
    Sock                 *sockPtr = NULL;
    Ns_Time               now;
    Tcl_Obj              *listObj = Tcl_NewListObj(0, NULL);
    NsConnChan           *connChanPtr = NULL;

    assert(arg != NULL);

    lcbPtr = arg;
    interp = Ns_TclAllocateInterp(lcbPtr->server);

    result = NSDriverSockNew(interp, sock, "http", lcbPtr->driverName, "CONNECTED", &sockPtr);

    if (result == TCL_OK) {
        Ns_GetTime(&now);
        connChanPtr = ConnChanCreate(sockPtr->servPtr,
                                     sockPtr,
                                     &now,
                                     NULL,
                                     NS_TRUE /* binary, fixed for the time being */,
                                     NULL);
        Ns_Log(Notice, "SockListenCallback new connChan %s sock %d", connChanPtr->channelName, sock);
    }

    if (connChanPtr != NULL) {
        Tcl_DString script;

        Tcl_DStringInit(&script);
        Tcl_DStringAppend(&script, lcbPtr->script, TCL_INDEX_NONE);
        Tcl_DStringAppendElement(&script, connChanPtr->channelName);
        result = Tcl_EvalEx(interp, script.string, script.length, 0);
        Tcl_DStringFree(&script);
        if (result != TCL_OK) {
            (void) Ns_TclLogErrorInfo(interp, "\n(context: connchan proc)");
        } else {
            Tcl_Obj *objPtr = Tcl_GetObjResult(interp);
            int      ok = 1;

            /*
             * The Tcl callback can signal with the result "0",
             * that the connection channel should be closed
             * automatically.
             */
            result = Tcl_GetBooleanFromObj(interp, objPtr, &ok);
            if ((result == TCL_OK) && (ok == 0)) {
                result = TCL_ERROR;
            }
        }
    }

    Ns_TclDeAllocateInterp(interp);
    Tcl_DecrRefCount(listObj);

    return (result == TCL_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanListObjCmd --
 *
 *      Implements the Tcl command "ns_connchan list", which returns a
 *      list of all active connection channels for the server. The
 *      function locks the connection channel table, iterates over
 *      each connection channel, and constructs a Tcl list containing
 *      key details for each channel, such as the channel name,
 *      associated thread name, start time, driver module, peer
 *      address, and bytes sent/received.
 *
 * Results:
 *      Returns TCL_OK if the command executes successfully (with the
 *      list of connection channels set as the Tcl interpreter
 *      result). Returns TCL_ERROR if argument parsing fails.
 *
 * Side effects:
 *      Acquires a read-lock on the connection channel table during iteration.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanListObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result = TCL_OK;
    Ns_ObjvSpec     lopts[] = {
        {"-server", Ns_ObjvServer, &servPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    }

    if (result == TCL_OK) {
        Tcl_HashSearch  search;
        Tcl_HashEntry  *hPtr;
        Tcl_DString     ds, *dsPtr = &ds;

        /*
         * The provided parameters appear to be valid. Lock the channel
         * table and return the infos for every existing entry in the
         * connection channel table.
         */
        Tcl_DStringInit(dsPtr);

        Ns_RWLockRdLock(&servPtr->connchans.lock);
        hPtr = Tcl_FirstHashEntry(&servPtr->connchans.table, &search);
        while (hPtr != NULL) {
            NsConnChan *connChanPtr;

            connChanPtr = (NsConnChan *)Tcl_GetHashValue(hPtr);
            Ns_DStringPrintf(dsPtr, "{%s %s " NS_TIME_FMT " %s %s %" PRIdz " %" PRIdz,
                             (char *)Tcl_GetHashKey(&servPtr->connchans.table, hPtr),
                             ((connChanPtr->cbPtr != NULL && connChanPtr->cbPtr->threadName != NULL) ?
                              connChanPtr->cbPtr->threadName : "{}"),
                             (int64_t) connChanPtr->startTime.sec, connChanPtr->startTime.usec,
                             connChanPtr->sockPtr->drvPtr->moduleName,
                             (*connChanPtr->peer == '\0' ? "{}" : connChanPtr->peer),
                             connChanPtr->wBytes,
                             connChanPtr->rBytes);
            Tcl_DStringAppendElement(dsPtr,
                                     (connChanPtr->clientData != NULL) ? connChanPtr->clientData : NS_EMPTY_STRING);
            /*
             * If we have a callback, write the cmd name. Rationale:
             * next arguments might contain already binary
             * data. Limitation: cmd name must not contain funny
             * characters.
             */
            if (connChanPtr->cbPtr != NULL) {
                char whenBuffer[6];

                Tcl_DStringAppend(dsPtr, " ", 1);
                Tcl_DStringAppend(dsPtr, connChanPtr->cbPtr->script, (TCL_SIZE_T)connChanPtr->cbPtr->scriptCmdNameLength);
                Tcl_DStringAppendElement(dsPtr, WhenToString(whenBuffer, connChanPtr->cbPtr->when));
            } else {
                Tcl_DStringAppend(dsPtr, " {} {}", 6);
            }

            /*
             * Terminate the list.
             */
            Tcl_DStringAppend(dsPtr, "} ", 2);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_RWLockUnlock(&servPtr->connchans.lock);

        Tcl_DStringResult(interp, dsPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanStatusObjCmd --
 *
 *      Implements "ns_connchan status".
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanStatusObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    char           *name = (char*)NS_EMPTY_STRING;
    int             result = TCL_OK;
    Ns_ObjvSpec     lopts[] = {
        {"-server", Ns_ObjvServer, &servPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     args[] = {
        {"channel", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    }

    if (result == TCL_OK) {
        NsConnChan *connChanPtr = ConnChanGet(interp, servPtr, name);

        if (connChanPtr != NULL) {
            Tcl_DString  ds;
            Tcl_Obj     *dictObj = Tcl_NewDictObj();

            Tcl_DStringInit(&ds);
            Ns_DStringPrintf(&ds, NS_TIME_FMT, (int64_t) connChanPtr->startTime.sec, connChanPtr->startTime.usec);

            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("start", 5),
                           Tcl_NewStringObj(ds.string, ds.length));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("driver", 6),
                           Tcl_NewStringObj(connChanPtr->sockPtr->drvPtr->moduleName, TCL_INDEX_NONE));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("peer", 4),
                           Tcl_NewStringObj(*connChanPtr->peer == '\0' ? "" : connChanPtr->peer, TCL_INDEX_NONE));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("sent", 4),
                           Tcl_NewWideIntObj((Tcl_WideInt)connChanPtr->wBytes));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("received", 8),
                           Tcl_NewWideIntObj((Tcl_WideInt)connChanPtr->rBytes));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("framebuffer", 8),
                           Tcl_NewIntObj(ConnChanBufferSize(connChanPtr,frameBuffer)));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("sendbuffer", 10),
                           Tcl_NewIntObj(ConnChanBufferSize(connChanPtr,sendBuffer)));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("fragments", 9),
                           Tcl_NewIntObj(ConnChanBufferSize(connChanPtr,fragmentsBuffer)));

            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("senderror", 9),
                           Tcl_NewStringObj(NsErrorCodeString((int)connChanPtr->sockPtr->sendErrno), TCL_INDEX_NONE));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("recverror", 9),
                           Tcl_NewStringObj(NsErrorCodeString((int)connChanPtr->sockPtr->recvErrno), TCL_INDEX_NONE));


            if (connChanPtr->cbPtr != NULL) {
                char whenBuffer[6] = {0};

                Tcl_DictObjPut(NULL, dictObj, Tcl_NewStringObj("callback", 8),
                               Tcl_NewStringObj(connChanPtr->cbPtr->script, TCL_INDEX_NONE));
                Tcl_DictObjPut(NULL, dictObj, Tcl_NewStringObj("condition", 9),
                               Tcl_NewStringObj(WhenToString(whenBuffer, connChanPtr->cbPtr->when), TCL_INDEX_NONE));
            }
            Tcl_DStringFree(&ds);
            Tcl_SetObjResult(interp, dictObj);

        } else {
            result = TCL_ERROR;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanCloseObjCmd --
 *
 *      Implements the Tcl command "ns_connchan close" to close a
 *      specified connection channel. This function looks up the
 *      connection channel by name, frees the associated resources,
 *      unregisters callbacks, and removes the channel from the
 *      server's connection channel table.
 *
 * Results:
 *      Returns TCL_OK if the connection channel is successfully closed;
 *      otherwise, returns TCL_ERROR.
 *
 * Side effects:
 *      Frees memory allocated for the connection channel structure,
 *      closes any associated sockets, and removes the channel's entry
 *      from the internal hash table.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanCloseObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    char           *name = (char*)NS_EMPTY_STRING;
    int             result = TCL_OK;
    Ns_ObjvSpec     lopts[] = {
        {"-server", Ns_ObjvServer, &servPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     args[] = {
        {"channel", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        NsConnChan *connChanPtr = ConnChanGet(interp, servPtr, name);

        Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan close connChanPtr %p", name, (void*)connChanPtr);

        if (connChanPtr != NULL && connChanPtr->debugLevel > 1) {
            char    fnbuffer[256];
            ssize_t bytes_written;

            snprintf(fnbuffer, sizeof(fnbuffer), "\n%" PRIxPTR " WRITE close,"
                     " total written %ld rejected %ld wbuffer %ld waddr %p\n",
                     Ns_ThreadId(), connChanPtr->wBytes, connChanPtr->sockPtr->sendRejected,
                     (long)ConnChanBufferSize(connChanPtr,sendBuffer),
                     ConnChanBufferAddress(connChanPtr,sendBuffer));
            bytes_written = write(connChanPtr->debugFD, fnbuffer, strlen(fnbuffer));
            (void)bytes_written;

            if (connChanPtr->debugFD != 0) {
                ns_close(connChanPtr->debugFD);
                connChanPtr->debugFD = 0;
            }
        }

        if (connChanPtr != NULL) {
            connChanPtr->debugLevel = 0;
            ConnChanFree(connChanPtr, servPtr);
        } else {
            result = TCL_ERROR;
        }

    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan close returns %s", name, Ns_TclReturnCodeString(result));
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanCallbackObjCmd --
 *
 *      Implements the Tcl command "ns_connchan callback". This
 *      command is used to register a new callback for a specified
 *      connection channel. The command accepts options to set
 *      timeouts for the callback, and it requires a channel name, a
 *      Tcl script (the callback), and a condition string ("when")
 *      indicating under which socket events the callback should be
 *      triggered.
 *
 *      The function parses the command-line arguments, retrieves the
 *      connection channel based on the provided channel name, and
 *      then registers the callback by invoking
 *      SockCallbackRegister(). If the callback registration fails,
 *      the connection channel is freed and an error is returned.
 *
 * Results:
 *      Returns a standard Tcl result: TCL_OK if the callback is
 *      registered successfully, or TCL_ERROR if there is an error in
 *      argument parsing or callback registration.
 *
 * Side effects:
 *      May modify the connection channel's state by updating its
 *      callback pointer, and if registration fails, frees the
 *      connection channel.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanCallbackObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int      result = TCL_OK;
    char    *name = (char*)NS_EMPTY_STRING;
    Ns_Time *pollTimeoutPtr = NULL, *recvTimeoutPtr = NULL, *sendTimeoutPtr = NULL;
    Tcl_Obj *whenObj, *scriptObj;
    Ns_ObjvSpec lopts[] = {
        {"-timeout",        Ns_ObjvTime, &pollTimeoutPtr, NULL},
        {"-receivetimeout", Ns_ObjvTime, &recvTimeoutPtr, NULL},
        {"-sendtimeout",    Ns_ObjvTime, &sendTimeoutPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"channel", Ns_ObjvString, &name,      NULL},
        {"command", Ns_ObjvObj,    &scriptObj, NULL},
        {"when",    Ns_ObjvObj,    &whenObj,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        NsConnChan     *connChanPtr = ConnChanGet(interp, servPtr, name);
        TCL_SIZE_T      whenStrlen;
        char           *whenString = Tcl_GetStringFromObj(whenObj, &whenStrlen);

        if (unlikely(connChanPtr == NULL)) {
            result = TCL_ERROR;
        } else if (whenStrlen == 0 || whenStrlen > 4) {

            Ns_TclPrintfResult(interp, "invalid when specification: \"%s\":"
                               " should be one/more of r, w, e, or x", whenString);
            result = TCL_ERROR;

        } else {
            /*
             * The provided channel name exists. In a first step get
             * the flags from the when string.
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
                    result = TCL_ERROR;
                    break;
                }
                s++;
            }

            if (result == TCL_OK) {
                Ns_ReturnCode status;

                Ns_RWLockWrLock(&servPtr->connchans.lock);

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
                 * Register the callback. This function call might set
                 * "connChanPtr->sockPtr = NULL;", therefore, we can't
                 * derive always the servPtr from the
                 * connChanPtr->sockPtr and we have to pass the
                 * servPtr to ConnChanFree().
                 */
                status = SockCallbackRegister(connChanPtr, scriptObj, when, pollTimeoutPtr);

                if (unlikely(status != NS_OK)) {
                    Ns_TclPrintfResult(interp, "could not register callback");
                    ConnChanFree(connChanPtr, servPtr);
                    result = TCL_ERROR;
                }
                Ns_RWLockUnlock(&servPtr->connchans.lock);
            }
        }
    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan callback returns %s", name, Ns_TclReturnCodeString(result));
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanExistsObjCmd --
 *
 *      Implements the Tcl command "ns_connchan exists". This command
 *      checks whether a connection channel with the specified name
 *      exists in the server's connection channel table.
 *
 * Results:
 *      Returns a standard Tcl result (TCL_OK) with a boolean Tcl object as the
 *      result: "1" if the connection channel exists, "0" otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanExistsObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char         *name = (char*)NS_EMPTY_STRING;
    int           result = TCL_OK;
    Ns_ObjvSpec   args[] = {
        {"channel", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp   *itPtr = clientData;
        NsServer         *servPtr = itPtr->servPtr;
        const NsConnChan *connChanPtr;

        connChanPtr = ConnChanGet(interp, servPtr, name);
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(connChanPtr != NULL));
    }

    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan exists returns %s", name, Ns_TclReturnCodeString(result));
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanReadBuffer --
 *
 *      Read from a connchan into a provided buffer.  In essence, this
 *      function performs timeout setup and handles NS_SOCK_AGAIN.
 *
 * Results:
 *      Number of bytes read or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static ssize_t
ConnChanReadBuffer(NsConnChan *connChanPtr, char *buffer, size_t bufferSize)
{
    ssize_t      nRead = 0;
    struct iovec buf;
    Ns_Time     *timeoutPtr, timeout;

    /*
     * Read the data via the "receive" operation of the driver.
     */
    buf.iov_base = buffer;
    buf.iov_len = bufferSize;

    timeoutPtr = &connChanPtr->recvTimeout;
    if (timeoutPtr->sec == 0 && timeoutPtr->usec == 0) {
        /*
         * No timeout was specified, use the configured receivewait of
         * the driver as timeout.
         */
        timeout.sec = connChanPtr->sockPtr->drvPtr->recvwait.sec;
        timeout.usec = connChanPtr->sockPtr->drvPtr->recvwait.usec;
        timeoutPtr = &timeout;
    }

    /*
     * In case we see an NS_SOCK_AGAIN, retry. We could make
     * this behavior optional via argument, but with OpenSSL,
     * this seems to happen quite often.
     */
    for (;;) {
        nRead = NsDriverRecv(connChanPtr->sockPtr, &buf, 1, timeoutPtr);
        Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan NsDriverRecv %" PRIdz
               " bytes recvSockState %.4x (driver %s)", connChanPtr->channelName, nRead,
               connChanPtr->sockPtr->recvSockState,
               connChanPtr->sockPtr->drvPtr->moduleName);
        if (nRead == 0 && connChanPtr->sockPtr->recvSockState == NS_SOCK_AGAIN) {
            continue;
        }
        break;
    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan NsDriverRecv %" PRIdz " bytes",
           connChanPtr->channelName, nRead);

    return nRead;
}

/*
 *----------------------------------------------------------------------
 *
 * RequireDsBuffer --
 *
 *      Ensures that the provided Tcl_DString pointer is initialized.
 *
 *      If the pointer is NULL, this function allocates memory for a new
 *      Tcl_DString and initializes it. This is used to guarantee that
 *      subsequent operations on the Tcl_DString can be performed safely.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May allocate memory for a new Tcl_DString if one is not already present.
 *
 *----------------------------------------------------------------------
 */
static void
RequireDsBuffer(Tcl_DString **dsPtr) {
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (*dsPtr == NULL) {
        *dsPtr = ns_malloc(sizeof(Tcl_DString));
        Tcl_DStringInit(*dsPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * WebsocketFrameSetCommonMembers --
 *
 *      Appends common WebSocket frame metadata to the provided Tcl dictionary
 *      object. This metadata includes:
 *         - "bytes": the total number of bytes read in the current operation.
 *         - "unprocessed": the length of the data in the frame buffer
 *           that has not yet been processed.
 *         - "fragments": the number of bytes stored in the fragments
 *           buffer, which holds parts of a multi-fragment message.
 *         - "havedata": a flag (0 or 1) indicating whether additional
 *           data is expected (i.e., whether the frame is complete or
 *           not).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The contents of the Tcl dictionary referenced by resultObj are updated.
 *
 *----------------------------------------------------------------------
 */
static void
WebsocketFrameSetCommonMembers(Tcl_Obj *resultObj, ssize_t nRead, const NsConnChan *connChanPtr)
{
    NS_NONNULL_ASSERT(resultObj != NULL);
    NS_NONNULL_ASSERT(connChanPtr != NULL);

    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("bytes", 5),
                   Tcl_NewLongObj((long)nRead));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("unprocessed", 11),
                   Tcl_NewIntObj(connChanPtr->frameBuffer->length));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("fragments", 9),
                   Tcl_NewIntObj(ConnChanBufferSize(connChanPtr,fragmentsBuffer)));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("havedata", 8),
                   Tcl_NewIntObj(!connChanPtr->frameNeedsData));
}

/*
 *----------------------------------------------------------------------
 *
 * GetWebsocketFrame --
 *
 *      Processes received data to extract a complete WebSocket frame.
 *
 *      This function appends newly received data to the connection channel's
 *      frame buffer and checks if it contains a complete WebSocket frame. It
 *      parses the frame header to determine the payload length and whether the
 *      frame is final (fin bit set) and masked. If the frame is incomplete,
 *      the function returns a Tcl dictionary indicating an "incomplete" frame.
 *      If the frame is complete, it handles unmasking (if necessary), assembles
 *      any fragmented payloads, and returns a Tcl dictionary that includes:
 *
 *          - "fin": a flag indicating if this is the final frame.
 *          - "frame": the frame status ("complete" if the full frame has been
 *             received; "incomplete" otherwise).
 *          - "opcode": the WebSocket opcode (present only for complete frames).
 *          - "payload": the full payload data of the frame.
 *
 *      The function also compacts the frame buffer to remove processed data,
 *      ensuring that any remaining unprocessed bytes are preserved for the next
 *      read.
 *
 * Results:
 *      A Tcl dictionary object representing the WebSocket frame. In
 *      the case of an incomplete frame, the dictionary will indicate
 *      that status; in case of a complete frame, it contains the
 *      opcode and payload.

 *
 * Side effects:
 *      May modify the connection channel's frame and fragments buffers.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj*
GetWebsocketFrame(NsConnChan *connChanPtr, char *buffer, ssize_t nRead)
{
    unsigned char *data;
    bool           finished, masked;
    int            opcode;
    TCL_SIZE_T     frameLength, fragmentsBufferLength;
    size_t         payloadLength, offset;
    unsigned char  mask[4] = {0u,0u,0u,0u};
    Tcl_Obj       *resultObj;

    resultObj = Tcl_NewDictObj();

    if (nRead < 0) {
        goto exception;
    }

    Ns_Log(Ns_LogConnchanDebug, "WS: received %ld bytes, have already %" PRITcl_Size,
           nRead, ConnChanBufferSize(connChanPtr, frameBuffer));
    /*
     * Make sure, the frame buffer exists.
     */
    RequireDsBuffer(&connChanPtr->frameBuffer);

    /*
     * Append the newly read data.
     */
    Tcl_DStringAppend(connChanPtr->frameBuffer, buffer, (TCL_SIZE_T)nRead);

    /*
     * On very small buffers, the interpretation of the first bytes
     * does not make sense. We need at least 2 bytes (on the
     * connections 6, including the mask).
     */
    if (connChanPtr->frameBuffer->length < 3) {
        frameLength = 0;
        goto incomplete;
    }

    /*
     * Check, if frame is complete.
     */
    data = (unsigned char *)connChanPtr->frameBuffer->string;

    finished      = ((data[0] & 0x80u) != 0);
    masked        = ((data[1] & 0x80u) != 0);
    opcode        = (data[0] & 0x0Fu);
    payloadLength = (data[1] & 0x7Fu);

    if (payloadLength <= 125) {
        offset = 2;
    } else if (payloadLength == 126) {
        uint16_t len16 = 0;

        memcpy(&len16, &data[2], 2);
        payloadLength = be16toh(len16);
        offset = 4;
    } else {
        uint64_t len64 = 0;

        memcpy(&len64, &data[2], 8);
        payloadLength = be64toh(len64);
        offset = 10;
    }

    if (masked) {
        /*
         * Initialize mask;
         */
        memcpy(&mask, &data[offset], 4);
        offset += 4;
    }

    frameLength = (TCL_SIZE_T)(offset + payloadLength);
    if (connChanPtr->frameBuffer->length < (TCL_SIZE_T)frameLength) {
        goto incomplete;
    }

    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("fin", 3), Tcl_NewIntObj(finished));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("frame", 5), Tcl_NewStringObj("complete", 8));

    if (!finished) {
        Ns_Log(Warning, "WS: unfinished frame, bytes %ld payload length %zu offset %zu "
               "avail %" PRITcl_Size " opcode %d fin %d, masked %d",
               nRead, payloadLength, offset, connChanPtr->frameBuffer->length,
               opcode, finished, masked);

        /*{   int i; for(i=0; i<connChanPtr->frameBuffer->length; i++) {
                fprintf(stderr,"%.2x",connChanPtr->frameBuffer->string[i]&0xFF);
            }
            fprintf(stderr, "\n");
            }*/
    }

    if (masked) {
        size_t i, j;

        for( i = offset, j = 0u; j < payloadLength; i++, j++ ) {
            data[ i ] = data[ i ] ^ mask[ j % 4];
        }
    }

    fragmentsBufferLength = ConnChanBufferSize(connChanPtr, fragmentsBuffer);

    if (finished) {
        Tcl_Obj *payloadObj;
        /*
         * The "fin" bit is set, this message is complete. If we have
         * fragments, append the new data to the fragments already
         * have received and clear the fragments buffer.
         */

        if (fragmentsBufferLength == 0) {
            payloadObj = Tcl_NewByteArrayObj(&data[offset], (TCL_SIZE_T)payloadLength);
        } else {
            Tcl_DStringAppend(connChanPtr->fragmentsBuffer,
                              (const char *)&data[offset], (TCL_SIZE_T)payloadLength);
            payloadObj = Tcl_NewByteArrayObj((const unsigned char *)connChanPtr->fragmentsBuffer->string,
                                             connChanPtr->fragmentsBuffer->length);
            Ns_Log(Ns_LogConnchanDebug,
                   "WS: append final payload opcode %d (fragments opcode %d) %" PRITcl_Size" bytes, "
                   "totaling %" PRITcl_Size " bytes, clear fragmentsBuffer",
                   opcode, connChanPtr->fragmentsOpcode,
                   (TCL_SIZE_T)payloadLength, connChanPtr->fragmentsBuffer->length);
            Tcl_DStringSetLength(connChanPtr->fragmentsBuffer, 0);
            opcode = connChanPtr->fragmentsOpcode;
        }
        Tcl_DictObjPut(NULL, resultObj,
                       Tcl_NewStringObj("opcode", 6),
                       Tcl_NewIntObj(opcode));
        Tcl_DictObjPut(NULL, resultObj,
                       Tcl_NewStringObj("payload", 7),
                       payloadObj);
    } else {
        /*
         * The "fin" bit is not set, we have a segment, but not the
         * complete message.  Append the received frame to the
         * fragments buffer.
         */
        RequireDsBuffer(&connChanPtr->fragmentsBuffer);
        /*
         * On the first fragment, keep the opcode since we will need
         * it for delivering the full message.
         */
        if (fragmentsBufferLength == 0) {
            connChanPtr->fragmentsOpcode = opcode;
        }
        Tcl_DStringAppend(connChanPtr->fragmentsBuffer,
                          (const char *)&data[offset], (TCL_SIZE_T)payloadLength);
        Ns_Log(Ns_LogConnchanDebug,
               "WS: fin 0 opcode %d (fragments opcode %d) "
               "append %" PRITcl_Size " to bytes to the fragmentsBuffer, "
               "totaling %" PRITcl_Size " bytes",
               opcode, connChanPtr->fragmentsOpcode,
               (TCL_SIZE_T)payloadLength, connChanPtr->fragmentsBuffer->length);
    }
    /*
     * Finally, compact the frameBuffer.
     */
    if (connChanPtr->frameBuffer->length > frameLength) {
        TCL_SIZE_T copyLength = connChanPtr->frameBuffer->length - frameLength;

        memmove(connChanPtr->frameBuffer->string,
                connChanPtr->frameBuffer->string + frameLength,
                (size_t)copyLength);
        Tcl_DStringSetLength(connChanPtr->frameBuffer, copyLength);
        //fprintf(stderr, "WS: leftover %d bytes\n", connChanPtr->frameBuffer->length);
        connChanPtr->frameNeedsData = NS_FALSE;
    } else {
        connChanPtr->frameNeedsData = NS_TRUE;
        Tcl_DStringSetLength(connChanPtr->frameBuffer, 0);
    }
    WebsocketFrameSetCommonMembers(resultObj, nRead, connChanPtr);
    return resultObj;

 incomplete:
    connChanPtr->frameNeedsData = NS_TRUE;
    Ns_Log(Notice, "WS: incomplete frameLength %" PRITcl_Size " avail %" PRITcl_Size,
           frameLength, connChanPtr->frameBuffer->length);
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("frame", 5), Tcl_NewStringObj("incomplete", 10));
    WebsocketFrameSetCommonMembers(resultObj, nRead, connChanPtr);
    return resultObj;

 exception:
    connChanPtr->frameNeedsData = NS_FALSE;
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("frame", 5),
                   Tcl_NewStringObj("exception", 10));
    WebsocketFrameSetCommonMembers(resultObj, nRead, connChanPtr);
    return resultObj;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanReadObjCmd --
 *
 *      Implements the "ns_connchan read" Tcl command, which reads
 *      data from a specified connection channel. The function
 *      supports both plain binary data and WebSocket frame
 *      processing. When the "-websocket" flag is provided and the
 *      channel is configured for WebSocket handling, it parses the
 *      received data into a complete WebSocket frame (if available)
 *      using GetWebsocketFrame().
 *
 *      In the non-WebSocket case, it simply reads data from the
 *      connection channel's socket into a byte array and returns it
 *      as a Tcl byte array object. If an error occurs during the read
 *      (for example, due to a receive timeout), an appropriate error
 *      message is set in the Tcl interpreter.
 *
 * Results:
 *      Returns TCL_OK on a successful read (with the read data set as
 *      the interpreter's result), or TCL_ERROR if an error occurs
 *      during the read operation.
 *
 * Side effects:
 *      - Updates internal counters for the number of bytes read.
 *      - May modify internal buffers of the connection channel to
 *        accumulate data.
 *      - Sets an error message and Tcl error code in case of read failures.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanReadObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char        *name = (char*)NS_EMPTY_STRING;
    int          result = TCL_OK, webSocketFrame = 0;
    Ns_ObjvSpec  opts[] = {
        {"-websocket", Ns_ObjvBool, &webSocketFrame, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"channel", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        NsConnChan     *connChanPtr = ConnChanGet(interp, servPtr, name);

        if (unlikely(connChanPtr == NULL)) {
            result = TCL_ERROR;
        } else {
            /*
             * The provided channel exists.
             */
            char         buffer[16384]; //buffer[16384];

            if (!connChanPtr->binary) {
                Ns_Log(Warning, "ns_connchan: only binary channels are currently supported. "
                       "Channel %s is not binary", name);
            }

            if ( webSocketFrame == 0 || connChanPtr->frameNeedsData) {
                ssize_t nRead = ConnChanReadBuffer(connChanPtr, buffer, sizeof(buffer));

                if (nRead < 0) {
                    const char *errorMsg;

                    errorMsg = NsSockSetRecvErrorCode(connChanPtr->sockPtr, interp);
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(errorMsg, TCL_INDEX_NONE));
                    result = TCL_ERROR;

                } else if (webSocketFrame == 0 && nRead > 0) {
                    connChanPtr->rBytes += (size_t)nRead;
                    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char *)buffer, (TCL_SIZE_T)nRead));
                } else if (webSocketFrame == 1) {
                    connChanPtr->rBytes += (size_t)nRead;
                    Tcl_SetObjResult(interp, GetWebsocketFrame(connChanPtr, buffer, nRead));
                } else {
                    /*
                     * The receive operation failed, maybe a receive
                     * timeout happened.  The read call will simply return
                     * an empty string. We could notice this fact
                     * internally by a timeout counter, but for the time
                     * being no application has usage for it.
                     */
                    /*Ns_Log(Notice, "ns_connchan read received no data, maybe a receive timeout or EOF");*/
                }
            } else {
                Tcl_SetObjResult(interp, GetWebsocketFrame(connChanPtr, buffer, 0));
            }
        }
    }

    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan read returns %s", name, Ns_TclReturnCodeString(result));

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * PrepareSendBuffers --
 *
 *      Prepares the data buffers for the send operation on a connection channel by
 *      organizing the new message data and any pre-existing buffered data into one or
 *      two iovec structures. The function handles several scenarios based on the current
 *      state of the connection channel:
 *
 *         - If a previous send operation was rejected by OpenSSL
 *           (i.e. sendRejected > 0), the function uses the stable
 *           buffer saved in sendRejectedBase for retransmission.  In
 *           this case, if new data is available, it is appended to
 *           the secondary send buffer.
 *
 *         - If the secondary send buffer already contains data, the
 *           new message is concatenated with that buffer, and the
 *           composite data is used as the message to be sent.
 *
 *         - If a stable send buffer is required
 *           (requireStableSendBuffer is true), the function ensures
 *           that the data (either from the existing send buffer
 *           combined with new data, or only new data) is stored in a
 *           stable (malloc()-ed) region to satisfy OpenSSL's
 *           retransmission requirements.
 *
 *         - In the default scenario, a two-buffer operation is set
 *           up: the first iovec references the prebuffered data,
 *           and the second iovec references the new message. If there
 *           is no existing buffered data, only the new message is
 *           used.
 *
 *      The function assigns the number of buffers used to nBuffers and sets an indicator in
 *      caseInt to reflect the selected strategy. It then returns the total number of bytes
 *      (toSend) that are scheduled for the send operation.
 *
 * Results:
 *      Returns a size_t value representing the total number of bytes to be sent.
 *
 * Side effects:
 *      - May modify the connection channel's sendBuffer and secondarySendBuffer (e.g., by
 *        appending new data).
 *      - Configures the iovec array (iovecs) to reference the correct buffer(s) for transmission.
 *
 *----------------------------------------------------------------------
 */

static size_t
PrepareSendBuffers(NsConnChan *connChanPtr, const char *msgString, TCL_SIZE_T msgLength,
                   struct iovec *iovecs, int *nBuffers, int *caseInt) {
    size_t toSend;

    /*
     * Handle rejected send.
     */
    if (connChanPtr->sockPtr->sendRejected > 0) {
        /*
         * A previous send operation was rejected by OpenSSL. OpenSSL
         * requires us to repeat the send operation with the same
         * pointer (data buffer) saved in sendRejectedBase and the
         * same length; The sendRejectedBase might differ from
         * ConnChanBufferAddress() in case some parts of the iovec
         * could be sent.
         *
         * In this situation, new incoming data must be stored in the
         * secondary buffer.
         */

        if (msgLength > 0) {
            RequireDsBuffer(&connChanPtr->secondarySendBuffer);
            Tcl_DStringAppend(connChanPtr->secondarySendBuffer, msgString, msgLength);
            Ns_Log(Notice, "REJECT HANDLING %s (%d,%ld): init secondary send buffer len %ld with msgLength %ld",
                   connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount,
                   (long)connChanPtr->secondarySendBuffer->length, (long)msgLength);
        }

        iovecs[0].iov_base = (void *)connChanPtr->sockPtr->sendRejectedBase;
        iovecs[0].iov_len = (size_t)connChanPtr->sockPtr->sendRejected;
        iovecs[1].iov_len = 0u;
        *nBuffers = 1;

        return iovecs[0].iov_len;
    }

    /*
     * Handle secondary buffer
     */
    if (ConnChanBufferSize(connChanPtr, secondarySendBuffer) > 0) {
        /*
         * If we have a nonempty secondary buffer, append the fresh
         * data to it and treat the content of the secondary buffer as
         * the new message.
         */
        Ns_Log(Notice, "REJECT HANDLING %s (%d,%ld): concatenate secondary buffer len %ld with msgLength %ld",
               connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount,
               (long)connChanPtr->secondarySendBuffer->length, (long)msgLength);
        Tcl_DStringAppend(connChanPtr->secondarySendBuffer, msgString, msgLength);

        msgString = connChanPtr->secondarySendBuffer->string;
        msgLength = connChanPtr->secondarySendBuffer->length;
    }

    /*
     * Prepare stable buffer.
     */
    if (msgLength > 0 && connChanPtr->requireStableSendBuffer) {
        /*
         * OpenSSL case - we need stable buffers. Use always the
         * sendbufer as a single, stable buffer. Make sure that in
         * the case, the send operation is rejected, no data is
         * shifted.
         */
        RequireDsBuffer(&connChanPtr->sendBuffer);

        if (ConnChanBufferSize(connChanPtr, sendBuffer) > 0) {
            /*
             *  We have both buffered data and new data.
             */
            Tcl_DStringAppend(connChanPtr->sendBuffer, msgString, msgLength);
            iovecs[0].iov_base = (void *)connChanPtr->sendBuffer->string;
            iovecs[0].iov_len = (size_t)connChanPtr->sendBuffer->length;
            *nBuffers = 1;
            *caseInt = 1;
        } else {
            /*
             * Only new data, but we need to copy it to stable buffer
             */
            Tcl_DStringAppend(connChanPtr->sendBuffer, msgString, msgLength);
            iovecs[0].iov_base = (void *)connChanPtr->sendBuffer->string;
            iovecs[0].iov_len = (size_t)msgLength;
            *nBuffers = 1;
            *caseInt = 3;
        }
        /*
         * Clear the secondary buffer.
         */
        if (ConnChanBufferSize(connChanPtr, secondarySendBuffer) > 0) {
            Tcl_DStringSetLength(connChanPtr->secondarySendBuffer, 0);
        }
        return iovecs[0].iov_len;
    }

    /*
     * Prepare buffered data
     */
    if (msgLength > 0 && ConnChanBufferSize(connChanPtr, sendBuffer) > 0) {
        /*
         * Case 1: New message exists and there is old buffered data.
         */
        *caseInt = 1;

        iovecs[0].iov_base = (void *)connChanPtr->sendBuffer->string;
        iovecs[0].iov_len = (size_t)connChanPtr->sendBuffer->length;
        iovecs[1].iov_base = (void *)msgString;
        iovecs[1].iov_len = (size_t)msgLength;
        *nBuffers = 2;
        toSend = (size_t)msgLength + (size_t)connChanPtr->sendBuffer->length;

        /*Ns_Log(Ns_LogConnchanDebug,
               "WS: send buffered only msgLength > 0, buf length %zu toSend %" PRIdz,
               iovecs[0].iov_len, toSend);*/

        if (connChanPtr->sockPtr->sendRejected > 0) {
            Ns_Log(Notice, "NsConnChanWrite sock %d has rejected data (%ld). New message exists and there is old buffered data",
                   connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
        }
    } else if (msgLength == 0 && ConnChanBufferSize(connChanPtr, sendBuffer) > 0) {
        /*
         * Case 2: No new data; only buffered data exists.
         */
        *caseInt = 2;
        iovecs[0].iov_base = (void *)connChanPtr->sendBuffer->string;
        iovecs[0].iov_len = (size_t)connChanPtr->sendBuffer->length;
        iovecs[1].iov_len = 0u;
        *nBuffers = 1;
        toSend = (size_t)connChanPtr->sendBuffer->length;
        /*Ns_Log(Ns_LogConnchanDebug,
               "WS: send buffered only msgLength == 0, buf length %zu toSend %" PRIdz,
               iovecs[0].iov_len, toSend);*/

        if (connChanPtr->sockPtr->sendRejected > 0) {
            Ns_Log(Notice, "NsConnChanWrite sock %d has rejected data (%ld). No new data; only buffered data exists",
                   connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
        }
    } else {
        /*
         * Case 3: No buffered data is available; use only the new message.
         */
        *caseInt = 3;
        iovecs[0].iov_base = (void *)msgString;
        iovecs[0].iov_len = (size_t)msgLength;
        iovecs[1].iov_len = 0u;
        *nBuffers = 1;
        /*Ns_Log(Ns_LogConnchanDebug, "WS: send msgLength toSend %ld", iovecs[0].iov_len);*/
        toSend = (size_t)msgLength;

        if (connChanPtr->sockPtr->sendRejected > 0) {
            Ns_Log(Notice, "NsConnChanWrite sock %d has rejected data (%ld). No buffered data is available; use only the new message",
                   connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
        }
    }

    return toSend;
}

/*
 *----------------------------------------------------------------------
 *
 * CompactSendBuffer --
 *
 *      Compacts the connection channel's send buffer by shifting the unsent data
 *      to the beginning of the buffer. The function copies data from the source
 *      specified by the iovec (iovecPtr->iov_base) to the start of the send buffer,
 *      and then updates the send buffer’s length to reflect the new data size.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - Modifies the content of the connection channel's send buffer.
 *      - Updates the buffer length to match the amount of data copied, effectively
 *        discarding the portion of data that was already sent.
 *
 *----------------------------------------------------------------------
 */
static void CompactSendBuffer(NsConnChan  *connChanPtr, struct iovec *iovecPtr)
{
    memmove(connChanPtr->sendBuffer->string,
            iovecPtr->iov_base,
            iovecPtr->iov_len);
    Tcl_DStringSetLength(connChanPtr->sendBuffer, (TCL_SIZE_T)iovecPtr->iov_len);
}

/*
 *----------------------------------------------------------------------
 *
 * DebugLogBufferState --
 *
 *      A variadic helper function to log buffer state information. It
 *      accepts a format string and any additional arguments.  if the
 *      connection channel's debug level is sufficiently high, it
 *      writes the message to the channel's debug file descriptor.
 *
 * Side Effects:
 *   Logs the formatted message and may write output to connChanPtr->debugFD.
 *
 *----------------------------------------------------------------------
 */
static void
DebugLogBufferState(NsConnChan *connChanPtr, size_t bytesToSend, ssize_t bytesSent, const char *data, const char *fmt, ...)
{
    if (connChanPtr->debugLevel > 1 && connChanPtr->debugFD > 0) {
        char    logMsg[512];
        va_list args;
        ssize_t bytes_written;

        snprintf(logMsg, sizeof(logMsg), "\n%" PRIxPTR " WRITE toSend %lu: ",
                 Ns_ThreadId(), bytesToSend);
        bytes_written = write(connChanPtr->debugFD, logMsg, strlen(logMsg));

        va_start(args, fmt);
        vsnprintf(logMsg, sizeof(logMsg), fmt, args);
        va_end(args);
        bytes_written = write(connChanPtr->debugFD, logMsg, strlen(logMsg));

        snprintf(logMsg, sizeof(logMsg), " total written %ld rejected %ld wbuffer %ld waddr %p\n",
                 connChanPtr->wBytes,
                 connChanPtr->sockPtr->sendRejected,
                 (long)ConnChanBufferSize(connChanPtr,sendBuffer),
                 ConnChanBufferAddress(connChanPtr,sendBuffer));
        bytes_written = write(connChanPtr->debugFD, logMsg, strlen(logMsg));

        if (data != NULL) {
            bytes_written = write(connChanPtr->debugFD, data, (size_t)bytesSent);
        }
        (void)bytes_written;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CompactBuffers --
 *
 *      This helper function "compacts" the connection channel's send buffer after a send
 *      operation by reordering its contents and determining how many bytes of new data remain
 *      unsent. The behavior depends on whether a dual-buffer send (i.e. combining old buffered
 *      data with new data) was used, or if only buffered or only fresh (new) data was sent.
 *
 *      In the dual-buffer case (nBuffers == 2):
 *
 *        1. When not all of the old buffered data was sent (i.e., oldBufferLen > bytesSent):
 *
 *           - The function shifts the unsent portion of the old
 *             buffer (pointed to by iovecs[0]) to the beginning of
 *             the send buffer.
 *           - Since none of the new data was transmitted in this
 *             scenario, the entire new message remains unsent. The
 *             return value unsentNewData is set to msgLength.
 *
 *        2. Otherwise, when all of the old buffered data was sent (and possibly some of the new data):
 *           - The function clears the send buffer.
 *           - It calculates the unsent portion of the new data as:
 *
 *                  unsentNewData = msgLength - ((TCL_SIZE_T)bytesSent - oldBufferLen)
 *
 *      In other cases:
 *
 *        - If only buffered data was sent (msgLength == 0): The
 *          function simply compacts the send buffer by shifting any
 *          remaining buffered data to the beginning. In this case,
 *          unsentNewData remains zero.
 *
 *        - If only fresh (new) data was sent (i.e., no buffered data
 *          is present): unsentNewData is computed as the difference
 *          between msgLength and bytesSent.
 *
 * Results:
 *      A value of type TCL_SIZE_T representing the number of bytes
 *      from the new message that remain unsent. This return value
 *      guides the caller on how much new data, if any, should be
 *      appended to the send buffer for future transmission attempts.
 *
 * Side Effects:

 *      - The function may modify the connection channel's send buffer
 *        (using memmove, Tcl_DStringSetLength, or Tcl_DStringAppend)
 *        to reflect the removal of sent data and the retention of
 *        unsent data.
 *      - Diagnostic messages are logged when debugging is enabled.
 *
 *----------------------------------------------------------------------
 */
static TCL_SIZE_T
CompactBuffers(NsConnChan *connChanPtr, const char *msgString, TCL_SIZE_T msgLength, ssize_t bytesSent,
               struct iovec *iovecs, int nBuffers, size_t toSend, int caseInt)
{
    TCL_SIZE_T unsentNewData = 0;

    if (nBuffers == 2) {
        TCL_SIZE_T bufferedDataLen = connChanPtr->sendBuffer->length;

        Ns_Log(Ns_LogConnchanDebug, "... two-buffer old buffer length %" PRITcl_Size " + new %" PRITcl_Size
               " = %" PRIdz " sent %ld (old not fully sent %d)",
               bufferedDataLen, msgLength, (size_t)bufferedDataLen + (size_t)msgLength, bytesSent,
               (bufferedDataLen > (TCL_SIZE_T)bytesSent));

        if (bufferedDataLen > (TCL_SIZE_T)bytesSent) {
            /*
             * Case 1: Not all of the old buffered data was sent. Move the
             * unsentBytes unsent portion to the start of the send
             * buffer. None of the new data was sent.
             *
             * iovecs[0].len is the unsent length,
             * iovecs[0].base points to the start of the unset buffer.
             */
            assert(iovecs[0].iov_len > 0);
            unsentNewData = msgLength;

            if (bytesSent > 0) {
                Ns_Log(Ns_LogConnchanDebug,
                       "... have sent part of old buffer %ld (BYTES from %" PRIdz " to %" PRIdz ")",
                       bytesSent,
                       connChanPtr->wBytes - (size_t)bytesSent,
                       connChanPtr->wBytes);

                DebugLogBufferState(connChanPtr, toSend, bytesSent, connChanPtr->sendBuffer->string,
                                    "sent part of buffer");

                CompactSendBuffer(connChanPtr, &iovecs[0]);

                if (connChanPtr->sockPtr->sendRejected > 0) {
                    Ns_Log(Notice, "NsConnChanWrite sock %d rejected data (%ld): "
                           "Shift remaining unsent data to the beginning of the buffer",
                           connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
                }
            }
        } else {
            /*
             * All of the old buffered data was sent (and maybe some fresh
             * data too).  Clear the send buffer and compute how many
             * bytes of the new message remain unsent.
             */
            assert(iovecs[0].iov_len == 0);
            Tcl_DStringSetLength(connChanPtr->sendBuffer, 0);

            if (connChanPtr->sockPtr->sendRejected > 0) {
                Ns_Log(Notice, "NsConnChanWrite sock %d rejected data (%ld): "
                       "toSend %ld, case %d, send buffer length %ld bytesSent %ld (length > bytesSent -> %d): "
                       "reset the send buffer",
                       connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected,
                       toSend, caseInt,
                       (long)bufferedDataLen, bytesSent,
                       bufferedDataLen > (TCL_SIZE_T)bytesSent);
            }

            unsentNewData = msgLength - ((TCL_SIZE_T)bytesSent - bufferedDataLen);
            Ns_Log(Ns_LogConnchanDebug,
                   "... have sent all of old buffer %" PRITcl_Size
                   " and %" PRITcl_Size " of new buffer "
                   "(BYTES from %" PRIdz " to %" PRIdz ")",
                   bufferedDataLen,
                   ((TCL_SIZE_T)bytesSent - bufferedDataLen),
                   connChanPtr->wBytes - (size_t)bytesSent,
                   connChanPtr->wBytes);

            if (connChanPtr->debugLevel > 1) {
                ssize_t bytes_written;

                (void)bytes_written;
                DebugLogBufferState(connChanPtr, toSend, bytesSent, connChanPtr->sendBuffer->string,
                                    "sent all from buffer + fresh data");
                bytes_written = write(connChanPtr->debugFD, msgString, (size_t)(bytesSent - bufferedDataLen));
            }
        }
    } else if (msgLength == 0) {
        /*
         * Only buffered data was available and sent (no new data).
         */
        assert(iovecs[0].iov_len > 0);

        unsentNewData = 0;
        Ns_Log(Ns_LogConnchanDebug,
               "... have sent from old buffer %" PRIdz " no new data "
               "(BYTES from %" PRIdz " to %" PRIdz ")",
               bytesSent,
               connChanPtr->wBytes - (size_t)bytesSent, connChanPtr->wBytes);

        if (connChanPtr->debugLevel > 1) {
            DebugLogBufferState(connChanPtr, toSend, bytesSent, connChanPtr->sendBuffer->string,
                                "sent all from buffer");
        }
        /*
         * Compact the send buffer by moving any unsent old data.
         */
        CompactSendBuffer(connChanPtr, &iovecs[0]);

        if (connChanPtr->sockPtr->sendRejected > 0) {
            Ns_Log(Notice, "NsConnChanWrite sock %d rejected data (%ld): "
                   "Only buffered data was available, moving unsent data",
                   connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
        }
    } else {
        /*
         * Only fresh data was sent (no buffered data present).
         */
        unsentNewData = msgLength - (TCL_SIZE_T)bytesSent;

        if (connChanPtr->debugLevel > 1) {
            DebugLogBufferState(connChanPtr, toSend, bytesSent, msgString,
                                "sent only fresh data");
        }
        if (connChanPtr->debugLevel > 0) {
            Ns_Log(Ns_LogConnchanDebug, "... have sent only fresh data %" PRIdz
                   " (BYTES from %" PRIdz " to %" PRIdz ")",
                   bytesSent, connChanPtr->wBytes - (size_t)bytesSent, connChanPtr->wBytes);
        }

        if (connChanPtr->sockPtr->sendRejected > 0) {
            Ns_Log(Notice, "NsConnChanWrite sock %d rejected data (%ld): "
                   "Only fresh data was sent",
                   connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
        }
    }

    return unsentNewData;
}


/*
 *----------------------------------------------------------------------
 *
 * NsConnChanWrite --
 *
 *      Sends data over a connection channel. This function writes the
 *      given message (or portion thereof) to the socket associated
 *      with the specified connection channel. If the connection
 *      channel already has data in its send buffer, the new message
 *      is appended to it and both buffers are sent together.
 *
 *      The function handles partial write operations and, if
 *      applicable, updates the connection channel's internal
 *      statistics for bytes sent.
 *
 * Parameters:
 *      interp      - The Tcl interpreter, used for error reporting.
 *      connChanName- The name of the connection channel as a null-terminated string.
 *      msgString   - Pointer to the message data to send.
 *      msgLength   - The length of the message data.
 *      nSentPtr    - Pointer to a variable where the total number of bytes sent
 *                    (across possibly multiple write attempts) will be stored.
 *      errnoPtr    - Pointer to a variable where the last error number from the
 *                    socket operation will be stored in case of an error.
 *
 * Results:
 *      Returns TCL_OK if the message is sent successfully (or partially sent with
 *      no fatal errors), or TCL_ERROR if an error occurs during the send operation.
 *
 * Side Effects:
 *      - May update the connection channel's send buffer by appending unsent data.
 *      - Increments the sent-bytes counter stored in the connection channel.
 *      - Sets the Tcl interpreter's result and error code on error.
 *
 *----------------------------------------------------------------------
 */
int
NsConnChanWrite(Tcl_Interp *interp, const char *connChanName, const char *msgString, TCL_SIZE_T msgLength,
                ssize_t *bytesSentPtr, unsigned long *errnoPtr)
{
    const NsInterp *itPtr = NsGetInterpData(interp);
    NsServer    *servPtr;
    NsConnChan  *connChanPtr;
    int          result = TCL_OK;
    ssize_t      bytesSent = 0, bytes_written;

    (void)bytes_written;

    servPtr = itPtr->servPtr;
    connChanPtr = ConnChanGet(interp, servPtr, connChanName);

    if (unlikely(connChanPtr == NULL)) {
        /*
         * If the connection channel doesn't exist, set error and return.
         */
        *errnoPtr = 0;
        result = TCL_ERROR;
    } else {
        /*
         * The provided channel name exists.
         */
        struct iovec iovecs[2];
        int          nBuffers = 1;
        size_t       toSend;
        int          caseInt = -1;

        iovecs[0].iov_len = 0;
        iovecs[1].iov_len = 0;

        if (connChanPtr->debugLevel > 1 && connChanPtr->debugFD == 0) {
            static char  fnbuffer[256];

            snprintf(fnbuffer, sizeof(fnbuffer), "/tmp/OUT-%s-XXXXXX", connChanName);
            connChanPtr->debugFD = ns_mkstemp(fnbuffer);
            Ns_Log(Notice, "CREATED file %s fd %d", fnbuffer, connChanPtr->debugFD);
        }

        toSend = PrepareSendBuffers(connChanPtr, msgString, msgLength, iovecs, &nBuffers, &caseInt);

        Ns_Log(Ns_LogConnchanDebug, "%s new message length %" PRITcl_Size
               " buffered length %" PRITcl_Size
               " total %" PRIdz,
               connChanName, msgLength, connChanPtr->sendBuffer != NULL ? connChanPtr->sendBuffer->length : 0,
               toSend);

        /*
         * Perform the send operation as registered in the driver.
         */
        bytesSent = (toSend > 0)
            ? ConnchanDriverSend(interp, connChanPtr, iovecs, nBuffers, 0u, &connChanPtr->sendTimeout)
            : 0;

        if (bytesSent != 0 && connChanPtr->sockPtr->sendRejected != 0) {
            Ns_Log(Warning, "REJECT HANDLING %s (%d,%ld): something was sent (%ld) but send rejected is still %ld, toSend %ld send buffer %p sendreject base %p",
                   connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount,
                   bytesSent, connChanPtr->sockPtr->sendRejected, toSend,
                   ConnChanBufferAddress(connChanPtr,sendBuffer),
                   connChanPtr->sockPtr->sendRejectedBase);
        }

        Ns_Log(Ns_LogConnchanDebug, "%s after ConnchanDriverSend nbufs %d len[0] %" PRIdz
            ", len[1] %" PRIdz " sent %" PRIdz,
            connChanName, nBuffers, iovecs[0].iov_len, iovecs[1].iov_len, bytesSent);

        /*
         * If some data was sent (bytesSent > -1), update the state accordingly.
         */
        if (bytesSent > -1) {
            size_t unsentBytes = (size_t)toSend - (size_t)bytesSent;

            if (connChanPtr->debugLevel > 1 && connChanPtr->sendBuffer != NULL) {
                DebugLogBufferState(connChanPtr, toSend, bytesSent, connChanPtr->sendBuffer->string,
                                    "partial write, unsent bytes %ld", unsentBytes);
                bytes_written = write(connChanPtr->debugFD, "\n-----CUT-HERE-----\n", 20u);
            }

            connChanPtr->wBytes += (size_t)bytesSent;

            if (unsentBytes > 0) {

                if (bytesSent == 0 && connChanPtr->requireStableSendBuffer) {
                    /*
                     * Nothing was sent.  In case, we require a stable
                     * send buffer, we have always a send buffer and
                     * always a single iovec. There is nothing to
                     * shift or concatenate.
                     */

                    if (connChanPtr->debugLevel > 1 && connChanPtr->sendBuffer != NULL) {
                        DebugLogBufferState(connChanPtr, toSend, bytesSent, connChanPtr->sendBuffer->string,
                                            "nothing was sent, unsent bytes %ld", unsentBytes);
                    }
                    if (connChanPtr->debugLevel > 0) {
                        Ns_Log(Notice, "REJECT HANDLING %s (%d,%ld): nothing was sent, send buffer %p sendreject base %p",
                               connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount,
                               ConnChanBufferAddress(connChanPtr,sendBuffer),
                               connChanPtr->sockPtr->sendRejectedBase);
                    }

                } else {
                    TCL_SIZE_T unsentNewData;

                    if (connChanPtr->debugLevel > 0) {
                        Ns_Log(Notice, "REJECT HANDLING %s (%d,%ld): something was sent (%ld), send buffer %p sendreject base %p, will call CompactBuffers",
                               connChanPtr->channelName, connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendCount,
                               bytesSent,
                               ConnChanBufferAddress(connChanPtr,sendBuffer),
                               connChanPtr->sockPtr->sendRejectedBase);
                    }
                    /*
                     * Something was sent.  Ensure that the send
                     * buffer is properly allocated.
                     */
                    if (connChanPtr->sendBuffer == NULL && connChanPtr->sockPtr->sendRejected > 0) {
                        Ns_Log(Notice, "NsConnChanWrite sock %d acquires send buffer with rejected data (%ld)",
                               connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
                    }
                    RequireDsBuffer(&connChanPtr->sendBuffer);

                    /*
                     * Compact old data in the sendBuffer.  If two buffers
                     * were used, determine how much of the new data has to be appended.
                     */
                    unsentNewData = CompactBuffers(connChanPtr, msgString, msgLength, bytesSent, iovecs, nBuffers, toSend, caseInt);

                    /*
                     * If there is unsent new data, append it to the
                     * send buffer for later transmission.
                     */
                    if (unsentNewData > 0) {
                        Tcl_DStringAppend(connChanPtr->sendBuffer,
                                          msgString + (msgLength - unsentNewData),
                                          unsentNewData);

                        if (connChanPtr->sockPtr->sendRejected > 0) {
                            Ns_Log(Notice, "NsConnChanWrite sock %d rejected data (%ld): "
                                   "append unsent new data",
                                   connChanPtr->sockPtr->sock, connChanPtr->sockPtr->sendRejected);
                        }
                    }
                }
            } else {
                /*
                 * All data was sent successfully.
                 */
                TCL_SIZE_T buffedLen = ConnChanBufferSize(connChanPtr, sendBuffer);

                Ns_Log(Ns_LogConnchanDebug, "... buffedLen %" PRITcl_Size
                       " msgLength %" PRITcl_Size
                       " everything was sent, unsentBytes %" PRIdz
                       ", (BYTES from %" PRIdz " to %" PRIdz ")",
                       buffedLen, msgLength, unsentBytes,
                       connChanPtr->wBytes - (size_t)bytesSent, connChanPtr->wBytes);
                assert(unsentBytes == 0);

                /*
                 * Clear the send buffer since all data was sent.
                 */

                if (buffedLen > 0) {
                    Tcl_DStringSetLength(connChanPtr->sendBuffer, 0);
                }
                if (connChanPtr->debugLevel > 1) {
                    DebugLogBufferState(connChanPtr, toSend, bytesSent, NULL, "all sent");
                }

            }
        } else {
            /*
             * The send operation failed, mark the result as an error.
             */
            result = TCL_ERROR;
        }

        /*
         * Update the error number from the socket's send error value.
         */
        *errnoPtr = connChanPtr->sockPtr->sendErrno;

    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan write returns %s", connChanName, Ns_TclReturnCodeString(result));

    /*
     * Update the output parameter for bytes sent.
     */
    *bytesSentPtr = bytesSent;
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanWriteObjCmd --
 *
 *      Implements "ns_connchan write", sending data over a connection
 *      channel. It determines whether to use buffered or unbuffered mode
 *      and writes the provided message accordingly.
 *
 * Results:
 *      Returns a standard Tcl result code (TCL_OK on success, TCL_ERROR on error).
 *
 * Side Effects:
 *      Writes data to the socket and updates connection channel statistics.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanWriteObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *name = (char*)NS_EMPTY_STRING;
    int         result = TCL_OK;
    Tcl_Obj    *msgObj;
    Ns_ObjvSpec args[] = {
        {"channel", Ns_ObjvString, &name,   NULL},
        {"message", Ns_ObjvObj,    &msgObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

#ifdef NS_WITH_DEPRECATED_5_0
    int buffered = 0;
    Ns_ObjvSpec  opts[] = {
        {"-buffered", Ns_ObjvBool, &buffered, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
#else
    Ns_ObjvSpec  opts[] = NULL;
#endif
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        ssize_t       bytesSent;
        TCL_SIZE_T    msgLength;
        unsigned long errorCode;
        const char   *msgString = (const char *)Tcl_GetByteArrayFromObj(msgObj, &msgLength);

#ifdef NS_WITH_DEPRECATED_5_0
        if (buffered != 0) {
            Ns_Log(Deprecated, "ns_connchan write: '-buffered' option is deprecated;"
                   " activated by default");
        }
#endif
        result = NsConnChanWrite(interp, name, msgString, msgLength, &bytesSent, &errorCode);
        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewLongObj((long)bytesSent));
        }
    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan write returns %s", name, Ns_TclReturnCodeString(result));
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanDebugObjCmd --
 *
 *      Implements "ns_connchan debug".
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanDebugObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    char           *name = (char*)NS_EMPTY_STRING;
    int             result = TCL_OK, debugLevel = -1;
    Ns_ObjvSpec     args[] = {
        {"channel", Ns_ObjvString, &name,   NULL},
        {"?level",  Ns_ObjvInt,    &debugLevel, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    }

    if (result == TCL_OK) {
        NsConnChan *connChanPtr = ConnChanGet(interp, servPtr, name);

        if (connChanPtr == NULL) {
            result = TCL_ERROR;
        } else {
            int oldDebugLevel = connChanPtr->debugLevel;

            if (debugLevel != -1) {
                connChanPtr->debugLevel = debugLevel;
            }
            Tcl_SetObjResult(interp, Tcl_NewIntObj(oldDebugLevel));
        }
    }

    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * ConnChanWsencodeObjCmd --
 *
 *      Implements "ns_connchan wsencode". Returns a WebSocket frame in
 *      form of binary data produced from the input parameters.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanWsencodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                      result = TCL_OK, isBinary = 0, opcode = 1, fin = 1, masked = 0;
    Tcl_Obj                 *messageObj;
    static Ns_ObjvTable      finValues[] = {
        {"0",  0u},
        {"1",  1u},
        {NULL, 0u}
    };
    static Ns_ObjvTable      opcodes[] = {
        {"continue",  0},
        {"text",      1},
        {"binary",    2},
        {"close",     8},
        {"ping",      9},
        {"pong",     10},
        {NULL,       0u}
    };
    Ns_ObjvSpec opts[] = {
        {"-binary",     Ns_ObjvBool,  &isBinary, INT2PTR(NS_TRUE)},
        {"-fin",        Ns_ObjvIndex, &fin,      &finValues},
        {"-mask",       Ns_ObjvBool,  &masked,   INT2PTR(NS_TRUE)},
        {"-opcode",     Ns_ObjvIndex, &opcode,   &opcodes},
        {"--",          Ns_ObjvBreak, NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"message", Ns_ObjvObj, &messageObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const unsigned char *messageString;
        unsigned char       *data;
        TCL_SIZE_T           messageLength;
        Tcl_DString          messageDs, frameDs;
        size_t               offset;

        Tcl_DStringInit(&messageDs);
        Tcl_DStringInit(&frameDs);

        /*
         * When the binary opcode is used, get as well the data in
         * form of binary data.
         */
        if (opcode == 2) {
            isBinary = 1;
        }
        messageString = Ns_GetBinaryString(messageObj, isBinary == 1, &messageLength, &messageDs);
        data = (unsigned char *)frameDs.string;

        Tcl_DStringSetLength(&frameDs, 2);
        /*
         * Initialize first two bytes, and then XOR flags into it.
         */
        data[0] = '\0';
        data[1] = '\0';

        data[0] = (unsigned char)(data[0] | ((unsigned char)opcode & 0x0Fu));
        if (fin) {
            data[0] |= 0x80u;
        }

        if ( messageLength <= 125 ) {
            data[1] = (unsigned char)(data[1] | ((unsigned char)messageLength & 0x7Fu));
            offset = 2;
        } else if ( messageLength <= 65535 ) {
            uint16_t len16;
            /*
             * Together with the first clause, this means:
             * messageLength > 125 && messageLength <= 65535
             */

            Tcl_DStringSetLength(&frameDs, 4);
            data[1] |= (( unsigned char )126 & 0x7Fu);
            len16 = htobe16((short unsigned int)messageLength);
            memcpy(&data[2], &len16, 2);
            offset = 4;
        } else {
            uint64_t len64;
            /*
             * Together with the first two clauses, this means:
             * messageLength > 65535
             */

            Tcl_DStringSetLength(&frameDs, 10);
            data[1] |= (( unsigned char )127 & 0x7Fu);
            len64 = htobe64((uint64_t)messageLength);
            memcpy(&data[2], &len64, 8);
            offset = 10;
        }

        if (masked) {
            unsigned char mask[4];
            size_t        i, j;

            data[1] |= 0x80u;
#ifdef HAVE_OPENSSL_EVP_H
            (void) RAND_bytes(&mask[0], 4);
#else
            {
                double d = Ns_DRand();
                /*
                 * In case double is 64-bits (which is the case on
                 * most platforms) the first four bytes contains much
                 * less randoness than the second 4 bytes.
                 */
                if (sizeof(d) == 8) {
                    const char *p = (const char *)&d;
                    memcpy(&mask[0], p+4, 4);
                } else {
                    memcpy(&mask[0], &d, 4);
                }
            }
#endif
            Tcl_DStringSetLength(&frameDs, (TCL_SIZE_T)offset + 4 + messageLength);
            data = (unsigned char *)frameDs.string;
            memcpy(&data[offset], &mask[0], 4);
            offset += 4;
            for( i = offset, j = 0u; j < (size_t)messageLength; i++, j++ ) {
                data[ i ] = messageString[ j ] ^ mask[ j % 4];
            }
        } else {
            Tcl_DStringSetLength(&frameDs, (TCL_SIZE_T)offset + messageLength);
            data = (unsigned char *)frameDs.string;
            memcpy(&data[offset], &messageString[0], (size_t)messageLength);
        }

        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(data, frameDs.length));

        Tcl_DStringFree(&messageDs);
        Tcl_DStringFree(&frameDs);
    }
    return result;
}




/*
 *----------------------------------------------------------------------
 *
 * NsTclConnChanObjCmd --
 *
 *      Implements the "ns_connchan" command, providing access to various
 *      operations on connection channels. This command accepts several
 *      subcommands (such as "connect", "close", "read", "write", etc.) and
 *      dispatches the call to the appropriate handler function.
 *
 * Results:
 *      Returns a standard Tcl result (TCL_OK on success or TCL_ERROR on failure).
 *
 * Side effects:
 *      May create, modify, or delete connection channel objects depending on the
 *      subcommand invoked.
 *
 *----------------------------------------------------------------------
 */

int
NsTclConnChanObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"callback", ConnChanCallbackObjCmd},
        {"connect",  ConnChanConnectObjCmd},
        {"close",    ConnChanCloseObjCmd},
        {"debug",    ConnChanDebugObjCmd},
        {"detach",   ConnChanDetachObjCmd},
        {"exists",   ConnChanExistsObjCmd},
        {"list",     ConnChanListObjCmd},
        {"listen",   ConnChanListenObjCmd},
        {"open",     ConnChanOpenObjCmd},
        {"read",     ConnChanReadObjCmd},
        {"status",   ConnChanStatusObjCmd},
        {"write",    ConnChanWriteObjCmd},
        {"wsencode", ConnChanWsencodeObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
