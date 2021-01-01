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
#elif defined(__APPLE__) || defined(_MSC_VER)
# define be16toh(x) ntohs(x)
# define htobe16(x) htons(x)
# define be32toh(x) ntonl(x)
# define htobe32(x) htonl(x)
# if defined(_MSC_VER)
/*
 * Not sure, why this did not work in Visual Studio 2019 (ntohll, htonll undefined)
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

/*
 * Structure handling one registered channel for the [ns_connchan]
 * command.
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
    Tcl_DString     *sendBuffer;       /* For unsent bytes in "ns_connchan write -buffered" */
    Tcl_DString     *frameBuffer;      /* Buffer of for a single WebSocket frame */
    Tcl_DString     *fragmentsBuffer;  /* Buffer for multiple WebSocket segments */
    int              fragmentsOpcode;  /* Opcode of the first WebSocket segment */
    bool             frameNeedsData;   /* Indicator, if additional reads are required */
} NsConnChan;

#define ConnChanBufferSize(connChanPtr, buf) ((connChanPtr)->buf != NULL ? (connChanPtr)->buf->length : 0)

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
 * Local functions defined in this file
 */
static Ns_ArgProc ArgProc;

static void CancelCallback(const NsConnChan *connChanPtr)
    NS_GNUC_NONNULL(1);

static NsConnChan *ConnChanCreate(NsServer *servPtr, Sock *sockPtr,
                                  const Ns_Time *startTime, const char *peer, bool binary,
                                  const char *clientData)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4)
    NS_GNUC_RETURNS_NONNULL;

static void ConnChanFree(NsConnChan *connChanPtr, NsServer *servPtr)
    NS_GNUC_NONNULL(1);

static NsConnChan *ConnChanGet(Tcl_Interp *interp, NsServer *servPtr, const char *name)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static Ns_ReturnCode SockCallbackRegister(NsConnChan *connChanPtr, const char *script,
                                          unsigned int when, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static ssize_t ConnchanDriverSend(Tcl_Interp *interp, const NsConnChan *connChanPtr,
                                  struct iovec *bufs, int nbufs, unsigned int flags,
                                  const Ns_Time *timeoutPtr
                                  ) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(6);

static char *WhenToString(char *buffer, unsigned int when)
    NS_GNUC_NONNULL(1);

static bool SockListenCallback(NS_SOCKET sock, void *arg, unsigned int UNUSED(why));
static void RequireDsBuffer(Tcl_DString **dsPtr)  NS_GNUC_NONNULL(1);

static Ns_SockProc NsTclConnChanProc;

static Tcl_ObjCmdProc   ConnChanCallbackObjCmd;
static Tcl_ObjCmdProc   ConnChanCloseObjCmd;
static Tcl_ObjCmdProc   ConnChanDetachObjCmd;
static Tcl_ObjCmdProc   ConnChanExistsObjCmd;
static Tcl_ObjCmdProc   ConnChanListObjCmd;
static Tcl_ObjCmdProc   ConnChanListenObjCmd;
static Tcl_ObjCmdProc   ConnChanOpenObjCmd;
static Tcl_ObjCmdProc   ConnChanReadObjCmd;
static Tcl_ObjCmdProc   ConnChanWriteObjCmd;
static Tcl_ObjCmdProc   ConnChanWsencodeObjCmd;

static Ns_SockProc CallbackFree;



/*
 *----------------------------------------------------------------------
 *
 * WhenToString --
 *
 *    Convert socket condition to character string.  The provided
 *    input buffer has to be at least 5 bytes long.
 *
 * Results:
 *    Pretty string.
 *
 * Side effects:
 *    None.
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
 *    Free Callback structure and unregister socket callback.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Freeing memory.
 *
 *----------------------------------------------------------------------
 */

static bool
CallbackFree(NS_SOCKET UNUSED(sock), void *arg, unsigned int why) {
    bool result;

    if (why != (unsigned int)NS_SOCK_CANCEL) {
        Ns_Log(Warning, "connchan CallbackFree called with unexpected reason code %u",
               why);
        result = NS_FALSE;

    } else {
        Callback *cbPtr = arg;

        Ns_Log(Ns_LogConnchanDebug, "connchan: callbackCallbackFree cbPtr %p why %u",
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
 *    Register socket callback cancel operation for unregistering the
 *    socket callback.  Freeing is itself implemented as a callback
 *    (Ns_SockProc), which is called, whenever a callback is freed
 *    from the socket thread. Not that it is necessary to implement it
 *    as a callback, since all sock callbacks are implemented via a
 *    queue operation (in sockcallback.c).
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
CancelCallback(const NsConnChan *connChanPtr)
{
    NS_NONNULL_ASSERT(connChanPtr != NULL);
    NS_NONNULL_ASSERT(connChanPtr->cbPtr != NULL);

    Ns_Log(Ns_LogConnchanDebug, "%s connchan: CancelCallback %p", connChanPtr->channelName, (void*)connChanPtr->cbPtr);

    (void)Ns_SockCancelCallbackEx(connChanPtr->sockPtr->sock, CallbackFree, connChanPtr->cbPtr, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanCreate --
 *
 *    Allocate a connection channel structure and initialize its fields.
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
    NS_NONNULL_ASSERT(peer != NULL);

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
    connChanPtr->frameBuffer = NULL;
    connChanPtr->fragmentsBuffer = NULL;
    connChanPtr->frameNeedsData = NS_TRUE;

    strncpy(connChanPtr->peer, peer, NS_IPADDR_SIZE - 1);
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
 *    Free NsConnChan structure and remove the entry from the hash
 *    table of open connection channel structures.
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
        if (connChanPtr->clientData != NULL) {
            ns_free((char *)connChanPtr->clientData);
        }

        if (connChanPtr->sockPtr != NULL) {
            NsSockClose(connChanPtr->sockPtr, (int)NS_FALSE);
            connChanPtr->sockPtr = NULL;
        }
        if (connChanPtr->sendBuffer != NULL) {
            Tcl_DStringFree(connChanPtr->sendBuffer);
            ns_free((char *)connChanPtr->sendBuffer);
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
 * ConnChanGet --
 *
 *    Access an NsConnChan from the per-server table via its name.
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
    const Tcl_HashEntry *hPtr;
    NsConnChan          *connChanPtr = NULL;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    servPtr = NsGetServer(nsconf.defaultServer);

    Ns_RWLockRdLock(&servPtr->connchans.lock);
    hPtr = Tcl_FindHashEntry(&servPtr->connchans.table, name);
    if (hPtr != NULL) {
        connChanPtr = (NsConnChan *)Tcl_GetHashValue(hPtr);
    }
    Ns_RWLockUnlock(&servPtr->connchans.lock);

    if (connChanPtr == NULL && interp != NULL) {
        Ns_TclPrintfResult(interp, "channel \"%s\" does not exist", name);
    }

    return connChanPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConnChanProc --
 *
 *      A wrapper function callback that is called, when the callback
 *      is fired. The function allocates an interpreter if necessary,
 *      builds the argument list for invocation and calls the
 *      registered Tcl script.
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
        char      whenBuffer[6];
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
            Tcl_DStringAppend(&script, cbPtr->script, (int)cbPtr->scriptLength);

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
                    Ns_DStringNAppend(&ds, script.string, (int)scriptCmdNameLength);
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
                         */
                        (void) Ns_SockCancelCallbackEx(localsock, NULL, NULL, NULL);
                    }
                } else {
                    Tcl_DStringInit(&ds);
                    Ns_DStringNAppend(&ds, script.string, (int)scriptCmdNameLength);

                    Ns_Log(Warning, "%s callback <%s> returned unhandled result '%s' (must be 0, 1, or 2)",
                           channelName,
                           ds.string,
                           Tcl_GetString(objPtr));
                    Tcl_DStringFree(&ds);
                }
            }
            if (channelName != NULL) {
                ns_free((char *)channelName);
            }
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
                servPtr = NsGetServer(nsconf.defaultServer);  // temporarily added
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
 *      Append info for socket callback.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
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
        Ns_DStringNAppend(dsPtr, cbPtr->connChanPtr->channelName, -1);
        Ns_DStringNAppend(dsPtr, " ", 1);
        Ns_DStringNAppend(dsPtr, cbPtr->script, (int)cbPtr->scriptCmdNameLength);
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
 *      Register a callback for the connection channel. Due to the
 *      underlying infrastructure, one socket has at most one callback
 *      registered.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      Memory management for the callback structure.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
SockCallbackRegister(NsConnChan *connChanPtr, const char *script,
                     unsigned int when, const Ns_Time *timeoutPtr)
{
    Callback     *cbPtr;
    size_t        scriptLength;
    Ns_ReturnCode result;
    const char   *p;

    NS_NONNULL_ASSERT(connChanPtr != NULL);
    NS_NONNULL_ASSERT(script != NULL);

    scriptLength = strlen(script);

    /*
     * If there is already a callback registered, free and cancel
     * it. This has to be done as first step, since CancelCallback()
     * calls finally Ns_SockCancelCallbackEx(), which deletes all
     * callbacks registered for the associated socket.
     */
    if (connChanPtr->cbPtr != NULL) {
        cbPtr = ns_realloc(connChanPtr->cbPtr, sizeof(Callback) + scriptLength);

    } else {
        cbPtr = ns_malloc(sizeof(Callback) + scriptLength);
    }
    memcpy(cbPtr->script, script, scriptLength + 1u);
    cbPtr->scriptLength = scriptLength;

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
        ssize_t nSent = 0, toSend = (ssize_t)Ns_SumVec(bufs, nbufs), origLength = toSend, partialResult;

        do {
            ssize_t partialToSend = (ssize_t)Ns_SumVec(bufs, nbufs);

            Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend try to send [0] %" PRIdz
                   " bytes (total %"  PRIdz ")",
                   connChanPtr->channelName,
                   bufs->iov_len, partialToSend);

            partialResult = NsDriverSend(sockPtr, bufs, nbufs, flags);
            Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend NsDriverSend returned result %"
                   PRIdz " errorState %d --- %s",
                   connChanPtr->channelName, partialResult, sockPtr->recvSockState, Tcl_ErrnoMsg(errno));

            if (partialResult == 0) {
                /*
                 * The resource is temporarily unavailable, we can an
                 * retry, when the socket is writable.
                 *
                 * If there is no timeout provided, return the bytes sent so far.
                 */
                if (timeoutPtr->sec == 0 && timeoutPtr->usec == 0) {
                    Ns_Log(Ns_LogConnchanDebug,
                           "%s ConnchanDriverSend would block, no timeout configured, "
                           "origLength %" PRIdz" still to send %" PRIdz " already sent %" PRIdz,
                           connChanPtr->channelName, origLength, toSend, nSent);
                    /*
                     * The partialResult might be between "0" and "partialToSend".
                     */
                    result = nSent;

                    break;
                } else {
                    /*
                     * A timeout was provided. Be aware that the timeout
                     * will suspend all sock-callback handlings for this
                     * time period.
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
                        Tcl_SetErrorCode(interp, "NS_TIMEOUT", (char *)0L);
                        Ns_Log(Ns_LogTimeoutDebug, "connchan send on %s runs into timeout",
                               connChanPtr->channelName);
                        partialResult = -1;
                    }
                }
            }

            partial = NS_FALSE;

            if (partialResult != -1) {
                nSent += partialResult;
                partialToSend -= partialResult;

                Ns_Log(Ns_LogConnchanDebug, "%s ConnchanDriverSend check partialResult %" PRIdz
                       " nSent %" PRIdz " toSend %" PRIdz " partial ? %d",
                       connChanPtr->channelName, partialResult, nSent, partialToSend, (partialToSend > 0));
                assert(partialToSend >= 0);

                if (partialToSend > 0) {
                    /*
                     * Partial write operation: part of the iovec has
                     * been sent, we have to retransmit the rest.
                     */
                    Ns_Log(Notice,
                           "%s ConnchanDriverSend partial write operation, sent %" PRIdz
                           " (so far %" PRIdz ") remaining %" PRIdz
                           " bytes, full length %" PRIdz,
                           connChanPtr->channelName, partialResult, nSent, partialToSend, origLength);
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
                Tcl_SetErrorCode(interp, "POSIX", Tcl_ErrnoId(), errorMsg, (char *)0L);
            }

            Ns_Log(Ns_LogConnchanDebug, "%s ### check result %ld == -1 || %ld == %ld "
                   "(partial %d && ok %d) => try again %d",
                   connChanPtr->channelName,
                   partialResult, toSend, nSent,
                   partial, (partialResult != -1), (partial && (partialResult != -1)));

        } while (partial && (partialResult != -1));

        if (partialResult != -1) {
            result = nSent;
        } else {
            result = -1;
        }

    } else {
        Ns_TclPrintfResult(interp, "channel %s: no sendProc registered for driver %s",
                           connChanPtr->channelName, sockPtr->drvPtr->moduleName);
        result = -1;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanDeatchObjCmd --
 *
 *    Implements "ns_connchan detach".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanDetachObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
        NsServer         *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
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

        Tcl_SetObjResult(interp, Tcl_NewStringObj(connChanPtr->channelName, -1));
        Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan detach returns %d", connChanPtr->channelName, result);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanOpenObjCmd --
 *
 *    Implements "ns_connchan open".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanOpenObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int           result;
    Sock         *sockPtr = NULL;
    Ns_Set       *hdrPtr = NULL;
    char         *url, *method = (char *)"GET", *version = (char *)"1.0",
                 *driverName = NULL, *sniHostname = NULL;
    Ns_Time       timeout = {1, 0}, *timeoutPtr = &timeout;
    Ns_ObjvSpec   lopts[] = {
        {"-driver",   Ns_ObjvString, &driverName, NULL},
        {"-headers",  Ns_ObjvSet,    &hdrPtr, NULL},
        {"-hostname", Ns_ObjvString, &sniHostname,    NULL},
        {"-method",   Ns_ObjvString, &method, NULL},
        {"-timeout",  Ns_ObjvTime,   &timeoutPtr,  NULL},
        {"-version",  Ns_ObjvString, &version, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   largs[] = {
        {"url", Ns_ObjvString, &url, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, largs, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        //const NsInterp *itPtr = clientData;
        NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
        NsConnChan     *connChanPtr;

        result = NSDriverClientOpen(interp, driverName, url, method, version, timeoutPtr, &sockPtr);
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
                    Ns_DriverClientInitArg params = {ctx, sniHostname};
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
                                             sockPtr->reqPtr->peer,
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
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(connChanPtr->channelName, -1));
                } else {
                    result = TCL_ERROR;
                }
            }
        }

        if (unlikely(result != TCL_OK && sockPtr != NULL && sockPtr->sock > 0)) {
            ns_sockclose(sockPtr->sock);
        }
        Ns_Log(Ns_LogConnchanDebug, "ns_connchan open %s returns %d", url, result);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanListenObjCmd --
 *
 *    Implements "ns_connchan listen".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */

static int
ConnChanListenObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    //const NsInterp *itPtr = clientData;
    NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
    int             result, doBind = (int)NS_FALSE;
    unsigned short  port = 0u;
    char           *driverName = NULL, *addr = (char*)NS_EMPTY_STRING, *script;
    Ns_ObjvSpec     lopts[] = {
        {"-driver",  Ns_ObjvString, &driverName, NULL},
        {"-server",  Ns_ObjvServer, &servPtr, NULL},
        {"-bind",    Ns_ObjvBool,   &doBind, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     largs[] = {
        {"address", Ns_ObjvString, &addr, NULL},
        {"port",    Ns_ObjvUShort, &port, NULL},
        {"script",  Ns_ObjvString, &script, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, largs, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        ListenCallback *lcbPtr;
        size_t          scriptLength;
        NS_SOCKET       sock;

        if (STREQ(addr, "*")) {
            addr = NULL;
        }
        scriptLength = strlen(script);
        lcbPtr = ns_malloc(sizeof(ListenCallback) + scriptLength);
        lcbPtr->server = servPtr->server;
        memcpy(lcbPtr->script, script, scriptLength + 1u);
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
                                             sockPtr->reqPtr->peer,
                                             NS_TRUE /* binary, fixed for the time being */,
                                             NULL);
                retVal = getsockname(sock, (struct sockaddr *) &sa, &len);
                if (retVal == -1) {
                    Ns_TclPrintfResult(interp, "can't obtain socket info %s", ns_sockstrerror(ns_sockerrno));
                    servPtr = NsGetServer(nsconf.defaultServer);  // temporarily added
                    ConnChanFree(connChanPtr, servPtr/*sockPtr->servPtr*/);
                    result = TCL_ERROR;
                } else {
                    Tcl_Obj  *listObj = Tcl_NewListObj(0, NULL);
                    char      ipString[NS_IPADDR_SIZE];

                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("channel", 7));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(connChanPtr->channelName, -1));

                    port = Ns_SockaddrGetPort((struct sockaddr *) &sa);
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("port", 4));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewIntObj((int)port));

                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("sock", 4));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewIntObj((int)sock));

                    ns_inet_ntop((struct sockaddr *) &sa, ipString, sizeof(ipString));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("address", 7));
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ipString, -1));

                    Tcl_SetObjResult(interp, listObj);
                }
            }
        }
    }
    Ns_Log(Ns_LogConnchanDebug, "ns_connchan listen %s %hu returns %d", addr, port, result);
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
 *      NS_TRUE or NS_FALSE on error
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
                                     sockPtr->reqPtr->peer,
                                     NS_TRUE /* binary, fixed for the time being */,
                                     NULL);
        Ns_Log(Notice, "SockListenCallback new connChan %s sock %d", connChanPtr->channelName, sock);
    }

    if (connChanPtr != NULL) {
        Tcl_DString script;

        Tcl_DStringInit(&script);
        Tcl_DStringAppend(&script, lcbPtr->script, -1);
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
 *    Implements "ns_connchan list".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    //const NsInterp *itPtr = clientData;
    NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
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
            Ns_DStringAppendElement(dsPtr,
                                    (connChanPtr->clientData != NULL) ? connChanPtr->clientData : NS_EMPTY_STRING);
            /*
             * If we have a callback, write the cmd name. Rationale:
             * next arguments might contain already binary
             * data. Limitation: cmd name must not contain funny
             * characters.
             */
            if (connChanPtr->cbPtr != NULL) {
                char whenBuffer[6];

                Ns_DStringNAppend(dsPtr, " ", 1);
                Ns_DStringNAppend(dsPtr, connChanPtr->cbPtr->script, (int)connChanPtr->cbPtr->scriptCmdNameLength);
                Ns_DStringAppendElement(dsPtr, WhenToString(whenBuffer, connChanPtr->cbPtr->when));
            } else {
                Ns_DStringNAppend(dsPtr, " {} {}", 6);
            }

            /*
             * Terminate the list.
             */
            Ns_DStringNAppend(dsPtr, "} ", 2);
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
 *    Implements "ns_connchan status".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanStatusObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    //const NsInterp *itPtr = clientData;
    NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
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
                           Tcl_NewStringObj(connChanPtr->sockPtr->drvPtr->moduleName, -1));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("peer", 4),
                           Tcl_NewStringObj(*connChanPtr->peer == '\0' ? "" : connChanPtr->peer, -1));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("sent", 4),
                           Tcl_NewWideIntObj((Tcl_WideInt)connChanPtr->wBytes));
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("reveived", 8),
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

            if (connChanPtr->cbPtr != NULL) {
                char whenBuffer[6];

                Tcl_DictObjPut(NULL, dictObj, Tcl_NewStringObj("callback", 8),
                               Tcl_NewStringObj(connChanPtr->cbPtr->script, -1));
                Tcl_DictObjPut(NULL, dictObj, Tcl_NewStringObj("condition", 9),
                               Tcl_NewStringObj(WhenToString(whenBuffer, connChanPtr->cbPtr->when), -1));
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
 *    Implements "ns_connchan close".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanCloseObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    //const NsInterp *itPtr = clientData;
    NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
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

        if (connChanPtr != NULL) {
            ConnChanFree(connChanPtr, servPtr);
        } else {
            result = TCL_ERROR;
        }

    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan close returns %d", name, result);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanCallbackObjCmd --
 *
 *    Implements "ns_connchan callback".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanCallbackObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int      result = TCL_OK;
    char    *name = (char*)NS_EMPTY_STRING,
            *script = (char*)NS_EMPTY_STRING,
            *whenString = (char*)NS_EMPTY_STRING;
    Ns_Time *pollTimeoutPtr = NULL, *recvTimeoutPtr = NULL, *sendTimeoutPtr = NULL;

    Ns_ObjvSpec lopts[] = {
        {"-timeout",        Ns_ObjvTime, &pollTimeoutPtr, NULL},
        {"-receivetimeout", Ns_ObjvTime, &recvTimeoutPtr, NULL},
        {"-sendtimeout",    Ns_ObjvTime, &sendTimeoutPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"channel", Ns_ObjvString, &name, NULL},
        {"script",  Ns_ObjvString, &script, NULL},
        {"when",    Ns_ObjvString, &whenString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        //const NsInterp *itPtr = clientData;
        NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
        NsConnChan     *connChanPtr = ConnChanGet(interp, servPtr, name);
        size_t          whenStrlen = strlen(whenString);

        assert(whenString != NULL);

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
                status = SockCallbackRegister(connChanPtr, script, when, pollTimeoutPtr);

                if (unlikely(status != NS_OK)) {
                    Ns_TclPrintfResult(interp, "could not register callback");
                    servPtr = NsGetServer(nsconf.defaultServer);  // temporarily added
                    ConnChanFree(connChanPtr, servPtr);
                    result = TCL_ERROR;
                }
                Ns_RWLockUnlock(&servPtr->connchans.lock);
            }
        }
    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan callback returns %d", name, result);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanExistsObjCmd --
 *
 *    Implements "ns_connchan exists".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanExistsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
        //const NsInterp   *itPtr = clientData;
        NsServer         *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
        const NsConnChan *connChanPtr;

        connChanPtr = ConnChanGet(interp, servPtr, name);
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(connChanPtr != NULL));
    }

    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan exists returns %d", name, result);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanReadBuffer --
 *
 *    Read from a connchan into a provided buffer.  In essence, this
 *    function performs timeout setup and handles NS_SOCK_AGAIN.
 *
 * Results:
 *    number of bytes read or -1 on error.
 *
 * Side effects:
 *    None.
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
 *    Make sure, the DS buffer is allocated.
 *
 * Results:
 *    None;
 *
 * Side effects:
 *    Potentially updates dsPtr which is passed as an argument
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
 * GetWebsocketFrame --
 *
 *     Frame handling for incoming WebSockets. This function checks,
 *     if the data read so far is a complete WebSocket frame
 *     (potentially consisting of multiple fragments) and returns the
 *     results in form of a Tcl dict. To handle partial frames or
 *     surplus data, the command performs socket level buffering based
 *     on Tcl_DStrings.
 *
 * Results:
 *     Tcl dict containing "fin" status bit, "frame" state (incomplete
 *     or complete), "unprocessed" (received data in buffer not
 *     handled so far), "haveData" (boolean value to express that
 *     unprocessed data might be sufficient for next frame.
 *
 *     In case the frame is finished, the dict contains as well the
 *     WebSocket "opcode" and "payload" of the frame.
 *
 * Side effects:
 *     None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj*
GetWebsocketFrame(NsConnChan *connChanPtr, char *buffer, ssize_t nRead)
{
    unsigned char *data;
    bool           finished, masked;
    int            opcode, frameLength, fragmentsBufferLength;
    size_t         payloadLength, offset;
    unsigned char  mask[4];
    Tcl_Obj       *resultObj;

    resultObj = Tcl_NewDictObj();
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("bytes", 5), Tcl_NewLongObj((long)nRead));
    if (nRead < 0) {
        goto exception;
    }

    Ns_Log(Ns_LogConnchanDebug, "WS: received %ld bytes, have already %d",
           nRead, ConnChanBufferSize(connChanPtr, frameBuffer));
    /*
     * Make sure, the frame buffer exists.
     */
    RequireDsBuffer(&connChanPtr->frameBuffer);

    /*
     * Append the newly read data.
     */

    //{int i; for(i=0; i<MIN(150, nRead); i++) {fprintf(stderr,"%.2x",buffer[i]&0xff);} fprintf(stderr, "\n");}
    Tcl_DStringAppend(connChanPtr->frameBuffer, buffer, (int)nRead);

    /*
     * On very small buffers, the interpretation of the first bytes
     * does not make sense. We need at least 2 bytes (on the
     * connections 6, including the mask).
     */
    if (connChanPtr->frameBuffer->length < 3) {
        frameLength = 0;
        goto incomplete;
    }

    //{int i; for(i=0; i<MIN(150, connChanPtr->frameBuffer->length); i++) {fprintf(stderr,"%.2x",connChanPtr->frameBuffer->string[i]&0xff);} fprintf(stderr, "\n");}
    /*
     * Check, if frame is complete.
     */
    data = (unsigned char *)connChanPtr->frameBuffer->string;

    finished      = ((data[0] & 0x80) != 0);
    masked        = ((data[1] & 0x80) != 0);
    opcode        = (data[0] & 0x0f);
    payloadLength = (data[1] & 0x7f);

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
    //fprintf(stderr, "WS: payload length %zu offset %zu avail %d opcode %d fin %d, masked %d MASK ",
    //        payloadLength, offset, connChanPtr->frameBuffer->length, opcode, finished, masked);
    //{int i; for(i=0; i<4; i++) {fprintf(stderr,"%.2x",mask[i]&0xff);} fprintf(stderr, "\n");}

    frameLength = (int)(offset + payloadLength);
    if (connChanPtr->frameBuffer->length < (int)frameLength) {
        //fprintf(stderr, "WS: INCOMPLETE offset %zu + payload length %zu = frameLength %d\n", offset, payloadLength, frameLength);
        //{int i; for(i=0; i<connChanPtr->frameBuffer->length; i++) {fprintf(stderr,"%.2x",connChanPtr->frameBuffer->string[i]&0xff);} fprintf(stderr, "\n");}
        goto incomplete;
    }
    //fprintf(stderr, "WS: COMPLETE ");
    //{int i; for(i=0; i<frameLength; i++) {fprintf(stderr,"%.2x",connChanPtr->frameBuffer->string[i]&0xff);} fprintf(stderr, "\n");}

    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("fin", 3), Tcl_NewIntObj(finished));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("frame", 5), Tcl_NewStringObj("complete", 8));

    if (!finished) {
        Ns_Log(Warning, "WS: unfinished frame, bytes %ld payload length %zu offset %zu avail %d opcode %d fin %d, masked %d",
               nRead, payloadLength, offset, connChanPtr->frameBuffer->length, opcode, finished, masked);
        {int i; for(i=0; i<connChanPtr->frameBuffer->length; i++) {fprintf(stderr,"%.2x",connChanPtr->frameBuffer->string[i]&0xff);} fprintf(stderr, "\n");}
    }

    if (masked) {
        size_t i, j;

        for( i = offset, j = 0u; j < payloadLength; i++, j++ ) {
            data[ i ] = data[ i ] ^ mask[ j % 4];
        }
        //fprintf(stderr, "\n");
        //{int i; for(i=offset; i<offset+payloadLength; i++) {fprintf(stderr,"%.2x",data[i]&0xff);} fprintf(stderr, "\n");}
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
            payloadObj = Tcl_NewByteArrayObj(&data[offset], (int)payloadLength);
        } else {
            Tcl_DStringAppend(connChanPtr->fragmentsBuffer, (const char *)&data[offset], (int)payloadLength);
            payloadObj = Tcl_NewByteArrayObj((const unsigned char *)connChanPtr->fragmentsBuffer->string,
                                             connChanPtr->fragmentsBuffer->length);
            Ns_Log(Ns_LogConnchanDebug,
                   "WS: append final payload opcode %d (fragments opcode %d) %d bytes, "
                   "totaling %d bytes, clear fragmentsBuffer",
                   opcode, connChanPtr->fragmentsOpcode,
                   (int)payloadLength, connChanPtr->fragmentsBuffer->length);
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
         * complete message.  Append the reveived frame to the
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
        Tcl_DStringAppend(connChanPtr->fragmentsBuffer, (const char *)&data[offset], (int)payloadLength);
        Ns_Log(Ns_LogConnchanDebug,
               "WS: fin 0 opcode %d (fragments opcode %d) "
               "append %d to bytes to the fragmentsBuffer, totaling %d bytes",
               opcode, connChanPtr->fragmentsOpcode,
               (int)payloadLength, connChanPtr->fragmentsBuffer->length);
    }
    /*
     * Finally, compact the frameBuffer.
     */
    if (connChanPtr->frameBuffer->length > frameLength) {
        int copyLength = connChanPtr->frameBuffer->length - frameLength;

        memcpy(connChanPtr->frameBuffer->string,
               connChanPtr->frameBuffer->string + frameLength,
               (size_t)copyLength);
        Tcl_DStringSetLength(connChanPtr->frameBuffer, copyLength);
        //fprintf(stderr, "WS: leftover %d bytes\n", connChanPtr->frameBuffer->length);
        connChanPtr->frameNeedsData = NS_FALSE;
    } else {
        connChanPtr->frameNeedsData = NS_TRUE;
        Tcl_DStringSetLength(connChanPtr->frameBuffer, 0);
    }
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("unprocessed", 11),
                   Tcl_NewIntObj(connChanPtr->frameBuffer->length));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("fragments", 9),
                   Tcl_NewIntObj(ConnChanBufferSize(connChanPtr,fragmentsBuffer)));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("havedata", 8),
                   Tcl_NewIntObj(!connChanPtr->frameNeedsData));
    return resultObj;

 incomplete:
    connChanPtr->frameNeedsData = NS_TRUE;
    Ns_Log(Notice, "WS: incomplete frameLength %d avail %d",
            frameLength, connChanPtr->frameBuffer->length);
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("frame", 5), Tcl_NewStringObj("incomplete", 10));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("unprocessed", 11), Tcl_NewIntObj(connChanPtr->frameBuffer->length));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("fragments", 9), Tcl_NewIntObj(ConnChanBufferSize(connChanPtr,fragmentsBuffer)));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("havedata", 8), Tcl_NewIntObj(!connChanPtr->frameNeedsData));

    return resultObj;

 exception:
    connChanPtr->frameNeedsData = NS_FALSE;
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("frame", 5), Tcl_NewStringObj("exception", 10));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("unprocessed", 11), Tcl_NewIntObj(connChanPtr->frameBuffer->length));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("fragments", 9), Tcl_NewIntObj(ConnChanBufferSize(connChanPtr,frameBuffer)));
    Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("havedata", 8), Tcl_NewIntObj(!connChanPtr->frameNeedsData));
    return resultObj;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanReadObjCmd --
 *
 *    Implements "ns_connchan read".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanReadObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
        //const NsInterp *itPtr = clientData;
        NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
        NsConnChan     *connChanPtr = ConnChanGet(interp, servPtr, name);

        if (unlikely(connChanPtr == NULL)) {
            result = TCL_ERROR;
        } else {
            /*
             * The provided channel exists.
             */
            ssize_t      nRead;
            char         buffer[16384]; //buffer[16384];

            if (!connChanPtr->binary) {
                Ns_Log(Warning, "ns_connchan: only binary channels are currently supported. "
                       "Channel %s is not binary", name);
            }

            if ( webSocketFrame == 0 || connChanPtr->frameNeedsData) {
                nRead = ConnChanReadBuffer(connChanPtr, buffer, sizeof(buffer));
                if (nRead < 0) {
                    const char *errorMsg;

                    errorMsg = NsSockSetRecvErrorCode(connChanPtr->sockPtr, interp);
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(errorMsg, -1));
                    result = TCL_ERROR;

                } else if (webSocketFrame == 0 && nRead > 0) {
                    connChanPtr->rBytes += (size_t)nRead;
                    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char *)buffer, (int)nRead));
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
                }
            } else {
                Tcl_SetObjResult(interp, GetWebsocketFrame(connChanPtr, buffer, 0));
            }
        }
    }

    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan read returns %d", name, result);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ConnChanWriteObjCmd --
 *
 *    Implements "ns_connchan write".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
ConnChanWriteObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char       *name = (char*)NS_EMPTY_STRING;
    int         result = TCL_OK, buffered = 0;
    Tcl_Obj    *msgObj;
    Ns_ObjvSpec  opts[] = {
        {"-buffered", Ns_ObjvBool, &buffered, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"channel", Ns_ObjvString, &name,   NULL},
        {"msg",     Ns_ObjvObj,    &msgObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        //const NsInterp *itPtr = clientData;
        NsServer       *servPtr = NsGetServer(nsconf.defaultServer); //itPtr->servPtr;
        NsConnChan     *connChanPtr = ConnChanGet(interp, servPtr, name);

        if (unlikely(connChanPtr == NULL)) {
            result = TCL_ERROR;
        } else {
            /*
             * The provided channel name exists.
             */
            struct iovec bufs[2];
            ssize_t      nSent;
            int          msgLen, nBufs = 1, toSend;
            const char  *msgString = (const char *)Tcl_GetByteArrayFromObj(msgObj, &msgLen);
#ifdef WS_RECORD_OUTPUT
            static int FD;
            static char fnbuffer[100];
#endif
            if (!connChanPtr->binary) {
                Ns_Log(Warning, "ns_connchan: only binary channels are currently supported. "
                       "Channel %s is not binary", name);
            }

#ifdef WS_RECORD_OUTPUT
            if (connChanPtr->wBytes == 0) {
                snprintf(fnbuffer, sizeof(fnbuffer), "/tmp/OUT-XXXXXX");
                mktemp(fnbuffer);
                FD = open("/tmp/OUT", O_APPEND | O_WRONLY | O_CREAT | O_TRUNC, 0644);
                fprintf(stderr, "CREATED file %s fd %d\n", fnbuffer, FD);
            }
#endif

            /*
             * When buffered was not specified, but we have a
             * sendbuffer, fall outmatically into buffered mode.
             */
            if (buffered == 0 && connChanPtr->sendBuffer != NULL) {
                Ns_Log(Notice, "ns_connchan send %s: force buffered", name);
                buffered = 1;
            }


            /*
             * Write the data via the "send" operation of the driver.
             */
            if (msgLen > 0 && buffered && ConnChanBufferSize(connChanPtr, sendBuffer) > 0) {
                bufs[0].iov_base = (void *)connChanPtr->sendBuffer->string;
                bufs[0].iov_len = (size_t)connChanPtr->sendBuffer->length;
                bufs[1].iov_base = (void *)msgString;
                bufs[1].iov_len = (size_t)msgLen;
                nBufs = 2;
                toSend = msgLen + connChanPtr->sendBuffer->length;
            } else if (msgLen == 0 && buffered && ConnChanBufferSize(connChanPtr, sendBuffer) > 0) {
                bufs[0].iov_base = (void *)connChanPtr->sendBuffer->string;
                bufs[0].iov_len = (size_t)connChanPtr->sendBuffer->length;
                bufs[1].iov_len = 0u;
                toSend = connChanPtr->sendBuffer->length;
                Ns_Log(Ns_LogConnchanDebug, "WS: send buffered only msgLen == 0, buf length %zu toSend %d", bufs[0].iov_len, toSend);
            } else {
                bufs[0].iov_base = (void *)msgString;
                bufs[0].iov_len = (size_t)msgLen;
                bufs[1].iov_len = 0u;
                Ns_Log(Ns_LogConnchanDebug, "WS: send msgLen toSend %ld", bufs[0].iov_len);
                toSend = msgLen;
            }

            /*Ns_Log(Notice, "WS: send buffered before nbufs %d len[0] % " PRIdz
                   ", len[1] %" PRIdz " len %d",
                   nBufs, bufs[0].iov_len, bufs[1].iov_len, toSend);*/

            if (toSend > 0) {
                nSent = ConnchanDriverSend(interp, connChanPtr, bufs, nBufs, 0u, &connChanPtr->sendTimeout);
            } else {
                nSent = 0;
            }

            /*Ns_Log(Notice, "WS: send buffered after  nbufs %d len[0] %" PRIdz
                   ", len[1] %" PRIdz " sent %" PRIdz,
                   nBufs, bufs[0].iov_len, bufs[1].iov_len, nSent);*/

            if (nSent > -1) {
                int remaining = toSend - (int)nSent;

                /*Ns_Log(Notice, "WS: send buffered %d msgLength %d nbufs %d to send %d sent %" PRIdz
                       " remaining %d errno %d %s (BYTES from %" PRIdz " to %" PRIdz ")",
                        buffered, msgLen, nBufs, toSend, nSent, remaining,
                        ns_sockerrno, ns_sockstrerror(ns_sockerrno),
                        connChanPtr->wBytes, connChanPtr->wBytes + (size_t)MIN(0,nSent));*/

                connChanPtr->wBytes += (size_t)nSent;
                Tcl_SetObjResult(interp, Tcl_NewLongObj((long)nSent));

                if (buffered && remaining > 0) {
                    int freshDataRemaining;

                    RequireDsBuffer(&connChanPtr->sendBuffer);
                    /*
                     * Compact old data. How much of the (old) sendBuffer was sent?
                     */
                    if (nBufs == 2) {
                        Ns_Log(Ns_LogConnchanDebug, "... two-buffer old buffer length %d + new %d"
                               " = %d sent %ld (old not fully sent %d)",
                                connChanPtr->sendBuffer->length, msgLen,
                                connChanPtr->sendBuffer->length + msgLen,
                                nSent, (connChanPtr->sendBuffer->length > nSent));
                        if (connChanPtr->sendBuffer->length > nSent) {
                            /*
                             * The old send buffer was not completely
                             * sent.
                             *
                             * bufs[0].len is the unsent length,
                             * bufs[0].base points to the begin of the
                             * unset buffer.
                             */
                            assert(bufs[0].iov_len > 0);

                            freshDataRemaining = msgLen;

                            if (nSent>0) {
                                Ns_Log(Ns_LogConnchanDebug,
                                       "... have sent part of old buffer %ld "
                                       "(BYTES from %" PRIdz " to %" PRIdz ")",
                                       nSent,
                                       connChanPtr->wBytes - (size_t)nSent,
                                       connChanPtr->wBytes);
#ifdef WS_RECORD_OUTPUT
                                write(FD, connChanPtr->sendBuffer->string, (size_t)nSent);
                                //write(2, connChanPtr->sendBuffer->string, (size_t)nSent);
                                fprintf(stderr, "\n");
#endif
                                memcpy(connChanPtr->sendBuffer->string,
                                       bufs[0].iov_base,
                                       bufs[0].iov_len);
                                Tcl_DStringSetLength(connChanPtr->sendBuffer, (int)bufs[0].iov_len);
                            }
                        } else {
                            /*
                             * The old send buffer was fully sent, and
                             * maybe some of the fresh data.
                             */
                            assert(bufs[0].iov_len == 0);
                            Tcl_DStringSetLength(connChanPtr->sendBuffer, 0);

                            freshDataRemaining = msgLen - (int)(nSent - connChanPtr->sendBuffer->length);
                            Ns_Log(Ns_LogConnchanDebug,
                                   "... have sent all of old buffer %d and %ld of new buffer "
                                   "(BYTES from %" PRIdz " to %" PRIdz ")",
                                   connChanPtr->sendBuffer->length,
                                   (nSent - connChanPtr->sendBuffer->length),
                                   connChanPtr->wBytes - (size_t)nSent, connChanPtr->wBytes);
#ifdef WS_RECORD_OUTPUT
                            write(FD, connChanPtr->sendBuffer->string, (size_t)connChanPtr->sendBuffer->length);
                            write(FD, msgString, (size_t)(nSent - connChanPtr->sendBuffer->length));
#endif
                        }
                    } else if (msgLen == 0) {
                        /*
                         * There was only some data from the sendBuffer, no new Data;
                         */
                        assert(bufs[0].iov_len > 0);

                        freshDataRemaining = 0;
                        Ns_Log(Ns_LogConnchanDebug,
                               "... have sent from old buffer %" PRIdz " no new data "
                               "(BYTES from %" PRIdz " to %" PRIdz ")",
                               nSent,
                               connChanPtr->wBytes - (size_t)nSent, connChanPtr->wBytes);
#ifdef WS_RECORD_OUTPUT
                        write(FD, connChanPtr->sendBuffer->string, (size_t)nSent);
#endif
                        memcpy(connChanPtr->sendBuffer->string,
                               bufs[0].iov_base,
                               bufs[0].iov_len);
                        Tcl_DStringSetLength(connChanPtr->sendBuffer, (int)bufs[0].iov_len);
                    } else {
                        /*
                         * There is only fresh data.
                         */
                        freshDataRemaining = msgLen - (int)nSent;
#ifdef WS_RECORD_OUTPUT
                        if (nSent > 0) {
                            write(FD, msgString, (size_t)nSent);
                            Ns_Log(Ns_LogConnchanDebug, "... have sent only fresh data %" PRIdz
                                   " (BYTES from %" PRIdz " to %" PRIdz ")",
                                   nSent,
                                   connChanPtr->wBytes - (size_t)nSent, connChanPtr->wBytes);
                        }
#endif
                    }

                    if (freshDataRemaining > 0) {
                        Tcl_DStringAppend(connChanPtr->sendBuffer,
                                          msgString + (msgLen - freshDataRemaining),
                                          freshDataRemaining);
                        Ns_Log(Ns_LogConnchanDebug, "... keep for later %d bytes of %d "
                               "(buffered %d) will be BYTES from %" PRIdz " to %" PRIdz,
                               freshDataRemaining, msgLen, connChanPtr->sendBuffer->length,
                               connChanPtr->wBytes,
                               connChanPtr->wBytes + (size_t)connChanPtr->sendBuffer->length);
                    }
                } else {
                    /*
                     * not (buffered && remaining > 0)
                     */
                    if (buffered) {
                        /*
                         * Everything was sent
                         */
                        int buffedLen = ConnChanBufferSize(connChanPtr, sendBuffer);
                        Ns_Log(Ns_LogConnchanDebug, "... buffered %d buffedLen %d msgLength %d "
                               "everything was sent, remaining %d, (BYTES from %" PRIdz " to %" PRIdz ")",
                               buffered, buffedLen, msgLen, remaining,
                               connChanPtr->wBytes - (size_t)nSent, connChanPtr->wBytes);
                        assert(remaining == 0);

                        if (buffedLen > 0) {
#ifdef WS_RECORD_OUTPUT
                            write(FD, connChanPtr->sendBuffer->string, (size_t)buffedLen);
#endif
                            Tcl_DStringSetLength(connChanPtr->sendBuffer, 0);
                        }
#ifdef WS_RECORD_OUTPUT
                        if (msgLen > 0) {
                            write(FD, msgString, (size_t)nSent);
                        }
#endif
                    } else {
                        /*
                         * Non-buffered case, there might be a partial send operation
                         */
                        if (remaining != 0) {
                            Ns_Log(Notice, "... partial write: to send %d sent %" PRIdz " remaining %d",
                                   toSend, nSent, remaining);
                        }
                    }
                }
            } else {
                result = TCL_ERROR;
            }
        }
    }
    Ns_Log(Ns_LogConnchanDebug, "%s ns_connchan write returns %d", name, result);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnChanWsencodeObjCmd --
 *
 *    Implements "ns_connchan wsencode". Returns a WebSocket frame in
 *    form of binary data produced from the input parameters.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

static int
ConnChanWsencodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                      result = TCL_OK, isBinary = 0, opcode = 1, fin = 1, masked = 0;
    static Ns_ObjvValueRange finRange = {0, 1};
    Tcl_Obj                 *messageObj;
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
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"-fin",        Ns_ObjvInt,   &fin,      &finRange},
        {"-mask",       Ns_ObjvBool,  &masked,   INT2PTR(NS_TRUE)},
        {"-opcode",     Ns_ObjvIndex, &opcode,   &opcodes},
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
        int                  messageLength;
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

        data[0] = (unsigned char)(data[0] | (unsigned char)(opcode & 0x0f));
        if (fin) {
            data[0] |= 0x80;
        }

        if ( messageLength <= 125 ) {
            data[1] = (unsigned char)(data[1] | ((unsigned char)messageLength & 0x7Fu));
            offset = 2;
        } else if ( messageLength > 125 && messageLength <= 65535 ) {
            uint16_t len16;

            Tcl_DStringSetLength(&frameDs, 4);
            data[1] |= (( unsigned char )126 & 0x7fu);
            len16 = htobe16((short unsigned int)messageLength);
            memcpy(&data[2], &len16, 2);
            offset = 4;
        } else {
            uint64_t len64;

            Tcl_DStringSetLength(&frameDs, 10);
            data[1] |= (( unsigned char )127 & 0x7fu);
            len64 = htobe64((uint64_t)messageLength);
            memcpy(&data[2], &len64, 8);
            offset = 10;
        }
        //{int i; fprintf(stderr, "masked %d length %d first two bytes: ", masked, messageLength); for(i=0; i<2; i++) {fprintf(stderr,"%.2x",data[i]&0xff);} fprintf(stderr, "\n");}

        if (masked) {
            unsigned char mask[4];
            size_t        i, j;

            data[1] |= 0x80;
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
            Tcl_DStringSetLength(&frameDs, (int)offset + 4 + messageLength);
            data = (unsigned char *)frameDs.string;
            memcpy(&data[offset], &mask[0], 4);
            offset += 4;
            for( i = offset, j = 0u; j < (size_t)messageLength; i++, j++ ) {
                data[ i ] = messageString[ j ] ^ mask[ j % 4];
            }
        } else {
            Tcl_DStringSetLength(&frameDs, (int)offset + messageLength);
            data = (unsigned char *)frameDs.string;
            memcpy(&data[offset], &messageString[0], (size_t)messageLength);
        }

        //{size_t i; for(i=0; i<(size_t)frameDs.length; i++) {fprintf(stderr,"%.2x",data[i]&0xff);} fprintf(stderr, "\n");}
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
 *    Implements "ns_connchan".
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
NsTclConnChanObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"callback", ConnChanCallbackObjCmd},
        {"close",    ConnChanCloseObjCmd},
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
