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
 * tclsock.c --
 *
 *          Tcl commands that let you do TCP socket operation.
 */

#include "nsd.h"

/*
 * The following structure is used for a socket callback.
 */

typedef struct Callback {
    const char  *server;
    Tcl_Channel  chan;
    unsigned int when;
    char         script[1];
} Callback;

/*
 * The following structure is used for a socket listen callback.
 */

typedef struct ListenCallback {
    const char *server;
    char  script[1];
} ListenCallback;

/*
 * Local functions defined in this file
 */

static int GetSet(Tcl_Interp *interp, const char *flist, int write,
                  fd_set **setPtrPtr, fd_set *setPtr, int *const maxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4)
    NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

static void AppendReadyFiles(Tcl_Interp *interp, Tcl_Obj *listObj, const fd_set *setPtr,
                             int write, const char *flist, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

static int EnterSock(Tcl_Interp *interp, NS_SOCKET sock, Tcl_Obj *listObj)
    NS_GNUC_NONNULL(1);
static int EnterDup(Tcl_Interp *interp, NS_SOCKET sock, Tcl_Obj *listObj)
    NS_GNUC_NONNULL(1);
static int EnterDupedSocks(Tcl_Interp *interp, NS_SOCKET sock, Tcl_Obj *listObj)
    NS_GNUC_NONNULL(1);

static int SockSetBlocking(const char *value, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ReturnCode GetSocketFromChannel(Tcl_Interp *interp, const char *chanId, int write, NS_SOCKET *socketPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static Ns_SockProc SockListenCallback;


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockArgProc --
 *
 *      Ns_ArgProc for info callback
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Appending script to the provided DString.
 *
 *----------------------------------------------------------------------
 */
void
NsTclSockArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const Callback *cbPtr = arg;

    Tcl_DStringAppendElement(dsPtr, cbPtr->script);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetHostObjCmd --
 *
 *      Performs a reverse DNS lookup.
 *      Implements "ns_hostbyaddr".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Puts a hostname into the Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
NsTclGetHostObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char       *addr;
    int         result = TCL_OK;
    Ns_ObjvSpec args[] = {
        {"address",  Ns_ObjvString, &addr,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_DString  ds;
        bool        success;

        Ns_DStringInit(&ds);
        success = Ns_GetHostByAddr(&ds, addr);

        if (success) {
            Tcl_DStringResult(interp, &ds);
        } else {
            Ns_TclPrintfResult(interp, "could not lookup %s", addr);
            result = TCL_ERROR;
        }
        Ns_DStringFree(&ds);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetAddrObjCmd --
 *
 *      Performs a DNS lookup.
 *      Implements "ns_addrbyhost".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Puts a single or multiple IP addresses the Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
NsTclGetAddrObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char       *host;
    int         all = 0, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-all",      Ns_ObjvBool,  &all, INT2PTR(NS_TRUE)},
        {"--",        Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"host",  Ns_ObjvString, &host,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        bool        success;
        Ns_DString  ds;

        Ns_DStringInit(&ds);
        if (all != 0) {
            success = Ns_GetAllAddrByHost(&ds, host);
        } else {
            success = Ns_GetAddrByHost(&ds, host);
        }
        if (success) {
            Tcl_DStringResult(interp, &ds);
        } else {
            Ns_TclPrintfResult(interp, "could not lookup %s", host);
            result = TCL_ERROR;
        }
        Ns_DStringFree(&ds);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockSetBlockingObjCmd --
 *
 *      Implements "ns_sockblocking". Sets a socket blocking.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockSetBlockingObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    return SockSetBlocking("1", interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockSetNonBlockingObjCmd --
 *
 *      Implements "ns_socknonblocking". Sets a socket nonblocking.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockSetNonBlockingObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    return SockSetBlocking("0", interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockNReadObjCmd --
 *
 *      Implements "ns_socknread". Gets the number of bytes that a
 *      socket has waiting to be read.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockNReadObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{

    int result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        result = TCL_ERROR;

    } else {
        unsigned long nread;
        NS_SOCKET     sock;
        Tcl_Channel   chan = Tcl_GetChannel(interp, Tcl_GetString(objv[1]), NULL);

        if (chan == NULL
            || GetSocketFromChannel(interp, Tcl_GetString(objv[1]), 0, &sock) != TCL_OK) {
            result = TCL_ERROR;

        } else if (ns_sockioctl(sock, FIONREAD, &nread) != 0) {
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                                   "ns_sockioctl failed: ",
                                   Tcl_PosixError(interp), (char *)0L);
            result = TCL_ERROR;

        } else {
            int nrBytes = (int)nread;

            nrBytes += Tcl_InputBuffered(chan);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(nrBytes));
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockListenObjCmd --
 *
 *      Listen on a TCP port.
 *      Implements "ns_socklisten".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Will listen on a port.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockListenObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char          *addr = (char*)NS_EMPTY_STRING;
    int            result;
    unsigned short port = 0u;
    Ns_ObjvSpec    args[] = {
        {"address", Ns_ObjvString, &addr, NULL},
        {"port",    Ns_ObjvUShort, &port, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        NS_SOCKET      sock;
        Tcl_Obj       *listObj = Tcl_NewListObj(0, NULL);

        if (STREQ(addr, "*")) {
            addr = NULL;
        }
        sock = Ns_SockListen(addr, port);
        if (sock == NS_INVALID_SOCKET) {
            Ns_TclPrintfResult(interp, "could not listen on [%s]:%hu",
                               Tcl_GetString(objv[1]), port);
            result = TCL_ERROR;
        } else {
            result = EnterSock(interp, sock, listObj);
        }
        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, listObj);
        } else {
            Tcl_DecrRefCount(listObj);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockAcceptObjCmd --
 *
 *      Accept a connection from a listening socket.
 *      Implements "ns_sockaccept".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockAcceptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    NS_SOCKET sock;
    int       result;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        result = TCL_ERROR;

    } else if (Ns_TclGetOpenFd(interp, Tcl_GetString(objv[1]), 0, (int *) &sock) != TCL_OK) {
        result = TCL_ERROR;

    } else {

        sock = Ns_SockAccept(sock, NULL, NULL);
        if (sock == NS_INVALID_SOCKET) {
            Ns_TclPrintfResult(interp, "accept failed: %s",
                               Tcl_PosixError(interp));
            result = TCL_ERROR;
        } else {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            result = EnterDupedSocks(interp, sock, listObj);
            if (result == TCL_OK) {
                Tcl_SetObjResult(interp, listObj);
            } else {
                Tcl_DecrRefCount(listObj);
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockCheckObjCmd --
 *
 *      Implements "ns_sockcheck". Checks if a socket is still
 *      connected, useful for nonblocking.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockCheckObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int        result = TCL_OK;
    Tcl_Obj   *objPtr;
    NS_SOCKET  sock;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        result = TCL_ERROR;

    } else if (Ns_TclGetOpenFd(interp, Tcl_GetString(objv[1]), 1, (int *) &sock) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        if (ns_send(sock, NULL, 0, 0) != 0) {
            objPtr = Tcl_NewBooleanObj(0);
        } else {
            objPtr = Tcl_NewBooleanObj(1);
        }

        Tcl_SetObjResult(interp, objPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockOpenObjCmd --
 *
 *      Open a tcp connection to a host/port.
 *      Implements "ns_sockopen".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Will open a connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockOpenObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char          *lhost = NULL, *host = (char*)NS_EMPTY_STRING;
    unsigned short lport = 0u, port = 0u;
    int            nonblock = 0, async = 0, result;
    Ns_Time       *timeoutPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-async",     Ns_ObjvBool,   &async,      INT2PTR(NS_TRUE)},
        {"-localhost", Ns_ObjvString, &lhost,      NULL},
        {"-localport", Ns_ObjvUShort, &lport,      NULL},
        {"-nonblock",  Ns_ObjvBool,   &nonblock,   INT2PTR(NS_TRUE)},
        {"-timeout",   Ns_ObjvTime,   &timeoutPtr, NULL},
        {"--",         Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"host",      Ns_ObjvString,  &host,       NULL},
        {"port",      Ns_ObjvUShort,  &port,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (*host == '\0') {
        Ns_TclPrintfResult(interp, "invalid hostname: must not be empty");
        result = TCL_ERROR;

    } else if (lhost != NULL && (*lhost == '\0')) {
        Ns_TclPrintfResult(interp, "invalid local hostname: must not be empty");
        result = TCL_ERROR;

    } else {
        NS_SOCKET      sock;
        Ns_ReturnCode  status = NS_OK;
        time_t         msec;

        /*
         * Provide error messages for invalid argument combinations.  Note that either
         *     -nonblock | -async
         * or
         *     -timeout time
         * are accepted as combinations.
         */
        if (nonblock != 0 || async != 0) {
            if (timeoutPtr != NULL) {
                Ns_TclPrintfResult(interp, "-timeout can't be specified when -async or -nonblock are used");
                return TCL_ERROR;
            }
            async = 1;
        }

        if (timeoutPtr != NULL) {
            msec = Ns_TimeToMilliseconds(timeoutPtr);
        } else {
            msec = -1;
        }

        /*
         * Perform the connection.
         */

        if (async != 0) {
            sock = Ns_SockAsyncConnect2(host, port, lhost, lport);
        } else if (msec < 0) {
            sock = Ns_SockConnect2(host, port, lhost, lport);
        } else {
            sock = Ns_SockTimedConnect2(host, port, lhost, lport, timeoutPtr, &status);
        }

        if (sock == NS_INVALID_SOCKET) {
            Ns_SockConnectError(interp, host, port, status);
            result = TCL_ERROR;

        } else {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            result = EnterDupedSocks(interp, sock, listObj);
            if (result == TCL_OK) {
                Tcl_SetObjResult(interp, listObj);
            } else {
                Tcl_DecrRefCount(listObj);
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSelectObjCmd --
 *
 *      Implements "ns_sockselect". Basically a Tcl version of select(2).
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSelectObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    fd_set                rset, wset, eset, *rPtr, *wPtr, *ePtr;
    int                   i, fobjc, status, arg, maxfd;
    Tcl_Channel           chan;
    struct timeval        tv, *tvPtr;
    Tcl_DString           dsRfd, dsNbuf;
    Tcl_Obj             **fobjv;
    Ns_Time               timeout;

    status = TCL_ERROR;

    if (objc != 6 && objc != 4) {
    syntax:
        Tcl_WrongNumArgs(interp, 1, objv, "?-timeout timeout? rfds wfds efds");
        return TCL_ERROR;
    }
    if (objc == 4) {
        tvPtr = NULL;
        arg = 1;
    } else {
        tvPtr = &tv;
        if (strcmp(Tcl_GetString(objv[1]), "-timeout") != 0) {
            goto syntax;
        }
        if (Ns_TclGetTimeFromObj(interp, objv[2], &timeout) != TCL_OK) {
            return TCL_ERROR;
        }
#ifdef _WIN32
        /*
         * Unfortunately, windows defines tv_sec as long.
         * https://docs.microsoft.com/en-us/windows/win32/api/winsock/ns-winsock-timeval
         */
        tv.tv_sec  = (long)timeout.sec;
#else
        tv.tv_sec  = timeout.sec;
#endif
        tv.tv_usec = (suseconds_t)timeout.usec;
        arg = 3;
    }

    /*
     * Readable fd's are treated differently because they may
     * have buffered input. Before doing a select, see if they
     * have any waiting data that's been buffered by the channel.
     */

    if (Tcl_ListObjGetElements(interp, objv[arg++], &fobjc, &fobjv) != TCL_OK) {
        return TCL_ERROR;
    }

    Tcl_DStringInit(&dsRfd);
    Tcl_DStringInit(&dsNbuf);
    for (i = 0; i < fobjc; ++i) {
        chan = Tcl_GetChannel(interp, Tcl_GetString(fobjv[i]), NULL);
        if (chan == NULL) {
            goto done;
        }
        if (Tcl_InputBuffered(chan) > 0) {
            Tcl_DStringAppendElement(&dsNbuf, Tcl_GetString(fobjv[i]));
        } else {
            Tcl_DStringAppendElement(&dsRfd, Tcl_GetString(fobjv[i]));
        }
    }

    if (dsNbuf.length > 0) {

        /*
         * Since at least one read fd had buffered input,
         * turn the select into a polling select just
         * to pick up anything else ready right now.
         */

        tv.tv_sec = 0;
        tv.tv_usec = 0;
        tvPtr = &tv;
    }
    maxfd = 0;
    if (GetSet(interp, dsRfd.string, 0, &rPtr, &rset, &maxfd) != TCL_OK) {
        goto done;
    }
    if (GetSet(interp, Tcl_GetString(objv[arg++]), 1, &wPtr, &wset, &maxfd) != TCL_OK) {
        goto done;
    }
    if (GetSet(interp, Tcl_GetString(objv[arg++]), 0, &ePtr, &eset, &maxfd) != TCL_OK) {
        goto done;
    }
    if (dsNbuf.length == 0 && rPtr == NULL && wPtr == NULL && ePtr == NULL && tvPtr == NULL) {

        /*
         * We're not doing a select on anything.
         */

        status = TCL_OK;

    } else {

        /*
         * Actually perform the select.
         */
        int nserrno, rc;

        do {
            rc = select(maxfd + 1, rPtr, wPtr, ePtr, tvPtr);
            nserrno = ns_sockerrno;
        } while (rc == -1 && nserrno == NS_EINTR);

        if (rc == -1) {
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "select failed: ",
                                   Tcl_PosixError(interp), (char *)0L);
        } else {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            if (rc == 0) {
                /*
                 * The sets can have any random value now
                 */

                if (rPtr != NULL) {
                    FD_ZERO(rPtr);
                }
                if (wPtr != NULL) {
                    FD_ZERO(wPtr);
                }
                if (ePtr != NULL) {
                    FD_ZERO(ePtr);
                }
            }
            AppendReadyFiles(interp, listObj, rPtr, 0, dsRfd.string, &dsNbuf);
            arg -= 2;
            AppendReadyFiles(interp, listObj, wPtr, 1, Tcl_GetString(objv[arg++]), NULL);
            AppendReadyFiles(interp, listObj, ePtr, 0, Tcl_GetString(objv[arg]), NULL);

            Tcl_SetObjResult(interp, listObj);
            status = TCL_OK;
        }
    }

done:
    Tcl_DStringFree(&dsRfd);
    Tcl_DStringFree(&dsNbuf);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSocketPairObjCmd --
 *
 *      Implements "ns_socketpair". Create a new socket pair.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSocketPairObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc),
                      Tcl_Obj *const* UNUSED(objv))
{
    NS_SOCKET socks[2];
    int       result;
    Tcl_Obj  *listObj = Tcl_NewListObj(0, NULL);


    if (ns_sockpair(socks) != 0) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                               "ns_sockpair failed:  ",
                               Tcl_PosixError(interp), (char *)0L);
        result = TCL_ERROR;

    } else if (EnterSock(interp, socks[0], listObj) != TCL_OK) {
        ns_sockclose(socks[1]);
        result = TCL_ERROR;

    } else {
        result = EnterSock(interp, socks[1], listObj);
    }
    if (result == TCL_OK) {
        Tcl_SetObjResult(interp, listObj);
    } else {
        Tcl_DecrRefCount(listObj);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockCallbackObjCmd --
 *
 *      Implements "ns_sockcallback". Register a Tcl callback to be
 *      run when a certain state exists on a socket.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      A callback will be registered.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockCallbackObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char           *script, *sockId, *whenString = (char*)NS_EMPTY_STRING;
    NS_SOCKET       sock;
    int             result = TCL_OK;
    size_t          scriptLength;
    Ns_Time        *timeoutPtr = NULL;
    unsigned int    when = 0u;
    Callback       *cbPtr;
    const NsInterp *itPtr = clientData;

    Ns_ObjvSpec args[] = {
        {"sockId",      Ns_ObjvString,  &sockId,       NULL},
        {"script",      Ns_ObjvString,  &script,       NULL},
        {"when",        Ns_ObjvString,  &whenString,   NULL},
        {"?timeout",    Ns_ObjvTime,    &timeoutPtr,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        while (*whenString != '\0') {
            if (*whenString == 'r') {
                when |= (unsigned int)NS_SOCK_READ;
            } else if (*whenString == 'w') {
                when |= (unsigned int)NS_SOCK_WRITE;
            } else if (*whenString == 'e') {
                when |= (unsigned int)NS_SOCK_EXCEPTION;
            } else if (*whenString == 'x') {
                when |= (unsigned int)NS_SOCK_EXIT;
            } else {
                Ns_TclPrintfResult(interp, "invalid when specification \"%s\": "
                                   "should be one/more of r, w, e, or x", whenString);
                result = TCL_ERROR;
                break;
            }
            ++whenString;
        }
    }

    if (result == TCL_OK && when == 0u) {
        Ns_TclPrintfResult(interp, "invalid when specification \"%s\": "
                           "should be one/more of r, w, e, or x", whenString);
        result = TCL_ERROR;

    } else if (GetSocketFromChannel(interp, sockId,
                                    (when & (unsigned int)NS_SOCK_WRITE) != 0u,
                                    &sock) != NS_OK) {
        result = TCL_ERROR;

    } else {
        if (timeoutPtr != NULL) {
            /*
             * timeout was specified, set is just in case the timeout was not 0:0
             */
            if (timeoutPtr->sec == 0 && timeoutPtr->usec == 0) {
                timeoutPtr = NULL;
            }
        }

        /*
         * Pass a dup of the socket to the callback thread, allowing
         * this thread's cleanup to close the current socket.  It is
         * not possible to simply register the channel again with
         * a NULL interp because the Tcl channel code is not entirely
         * thread safe.
         */

        sock = ns_sockdup(sock);
        scriptLength = strlen(script);

        cbPtr = ns_malloc(sizeof(Callback) + (size_t)scriptLength);
        cbPtr->server = (itPtr->servPtr != NULL ? itPtr->servPtr->server : NULL);
        cbPtr->chan = NULL;
        cbPtr->when = when;
        memcpy(cbPtr->script, script, (size_t)scriptLength + 1u);

        if (Ns_SockCallbackEx(sock, NsTclSockProc, cbPtr,
                              when | (unsigned int)NS_SOCK_EXIT,
                              timeoutPtr, NULL) != NS_OK) {
            Ns_TclPrintfResult(interp, "could not register callback");
            ns_sockclose(sock);
            ns_free(cbPtr);
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockListenCallbackObjCmd --
 *
 *      Listen on a socket and register a callback to run when
 *      connections arrive.
 *      Implements "ns_socklistencallback".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Will register a callback and listen on a socket.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSockListenCallbackObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char           *addr =  (char*)NS_EMPTY_STRING, *script = (char*)NS_EMPTY_STRING;
    unsigned short  port = 0u;
    int             result = TCL_OK;
    Ns_ObjvSpec     args[] = {
        {"address", Ns_ObjvString, &addr, NULL},
        {"port",    Ns_ObjvUShort, &port, NULL},
        {"script",  Ns_ObjvString, &script, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        ListenCallback *lcbPtr;
        size_t          scriptLength;

        assert(script != NULL);

        if (STREQ(addr, "*")) {
            addr = (char *)NS_IP_UNSPECIFIED;
        }
        scriptLength = strlen(script);
        lcbPtr = ns_malloc(sizeof(ListenCallback) + scriptLength);
        if (unlikely(lcbPtr == NULL)) {
            result = TCL_ERROR;
        } else {
            Ns_ReturnCode  status;

            lcbPtr->server = (itPtr->servPtr != NULL ? itPtr->servPtr->server : NULL);
            memcpy(lcbPtr->script, script, scriptLength + 1u);
            status = Ns_SockListenCallback(addr, port, SockListenCallback, NS_FALSE, lcbPtr);
            if (status == NS_INVALID_SOCKET) {
                Ns_TclPrintfResult(interp, "could not register callback");
                ns_free(lcbPtr);
                result = TCL_ERROR;
            }
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SockSetBlocking --
 *
 *      Set a socket blocking.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SockSetBlocking(const char *value, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result;

    NS_NONNULL_ASSERT(value != NULL);
    NS_NONNULL_ASSERT(interp != NULL);

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        result = TCL_ERROR;

    } else {

        Tcl_Channel chan = Tcl_GetChannel(interp, Tcl_GetString(objv[1]), NULL);
        if (chan == NULL) {
            result = TCL_ERROR;
        } else {
            result = Tcl_SetChannelOption(interp, chan, "-blocking", value);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AppendReadyFiles --
 *
 *      Find files in an fd_set that are selected and append them to
 *      the passed in list obj, and also an optional passed-in
 *      DString.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Ready files will be appended to the dsPtr if not null, and also
 *      to the provided list.
 *
 *----------------------------------------------------------------------
 */

static void
AppendReadyFiles(Tcl_Interp *interp, Tcl_Obj *listObj,
                 const fd_set *setPtr, int write, const char *flist,
                 Tcl_DString *dsPtr)
{
    int           fargc = 0;
    const char  **fargv = NULL;
    NS_SOCKET     sock;
    Tcl_DString   ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(listObj != NULL);
    NS_NONNULL_ASSERT(flist != NULL);

    Tcl_DStringInit(&ds);
    if (dsPtr == NULL) {
        dsPtr = &ds;
    }
    if (Tcl_SplitList(interp, flist, &fargc, &fargv) == TCL_OK) {
        while (fargc-- > 0) {
            Ns_ReturnCode rc = GetSocketFromChannel(interp, fargv[fargc], write, &sock);

            if ((rc == NS_OK) && (setPtr != NULL) && FD_ISSET(sock, setPtr)) {
                Tcl_DStringAppendElement(dsPtr, fargv[fargc]);
            }
        }

        /*
         * Append the ready files to the passed in listObj
         */
        Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(dsPtr->string, -1));

        Tcl_Free((char *) fargv);
    } else {
        Ns_Log(Error, "Can't split list '%s'", flist);
    }
    Tcl_DStringFree(&ds);
}

/*
 *----------------------------------------------------------------------
 *
 * GetSocketFromChannel --
 *
 *      Return the socket for the given channel.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      The value at socketPtr is updated with a valid NS_SOCKET.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
GetSocketFromChannel(Tcl_Interp *interp, const char *chanId, int write, NS_SOCKET *socketPtr)
{
    Tcl_Channel   chan;
    ClientData    data;
    Ns_ReturnCode result = NS_OK;
    NS_SOCKET     sock = NS_INVALID_SOCKET;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(chanId != NULL);
    NS_NONNULL_ASSERT(socketPtr != NULL);

    if (Ns_TclGetOpenChannel(interp, chanId, write, NS_TRUE, &chan) != TCL_OK) {
        result = NS_ERROR;

    } else if (Tcl_GetChannelHandle(chan, write != 0 ? TCL_WRITABLE : TCL_READABLE,
                                    &data) != TCL_OK) {
        Ns_TclPrintfResult(interp, "could not get handle for channel: %s", chanId);
        result = NS_ERROR;

    } else {
        sock = PTR2INT(data);
    }
    *socketPtr = sock;

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * GetSet --
 *
 *      Take a Tcl list of files and set bits for each in the list in
 *      the fd_set.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      set bits in fd_set. Max fd will be returned in "*maxPtr".
 *
 *----------------------------------------------------------------------
 */

static int
GetSet(Tcl_Interp *interp, const char *flist, int write, fd_set **setPtrPtr,
       fd_set *setPtr, int *const maxPtr)
{
    int          fargc, result;
    const char **fargv = NULL;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(flist != NULL);
    NS_NONNULL_ASSERT(setPtrPtr != NULL);
    NS_NONNULL_ASSERT(setPtr != NULL);
    NS_NONNULL_ASSERT(maxPtr != NULL);

    if (Tcl_SplitList(interp, flist, &fargc, &fargv) != TCL_OK) {
        result = TCL_ERROR;

    } else if (fargc == 0) {
        ckfree((char *)fargv);
        *setPtrPtr = NULL;
        result = TCL_OK;

    } else {
        *setPtrPtr = setPtr;

        FD_ZERO(setPtr);
        result = TCL_OK;

        /*
         * Loop over each file, get its FD and set the bit in the
         * fd_set.
         */

        while (fargc-- > 0) {
            NS_SOCKET sock = NS_INVALID_SOCKET;

            if (GetSocketFromChannel(interp, fargv[fargc],
                                     write, &sock) != NS_OK) {
                result = TCL_ERROR;
                break;
            }
#ifndef _MSC_VER
            /* winsock ignores first argument of select */
            if (sock > *maxPtr) {
                *maxPtr = sock;
            }
#endif
            assert(sock != NS_INVALID_SOCKET);

            FD_SET(sock, setPtr);
        }
        Tcl_Free((char *) fargv);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * EnterSock, EnterDup, EnterDupedSocks --
 *
 *      Append a socket handle to the Tcl result and register its
 *      channel.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Will create channel, append handle to result.
 *
 *----------------------------------------------------------------------
 */

static int
EnterSock(Tcl_Interp *interp, NS_SOCKET sock, Tcl_Obj *listObj)
{
    Tcl_Channel chan;
    int result;

    NS_NONNULL_ASSERT(interp != NULL);

    chan = Tcl_MakeTcpClientChannel(INT2PTR(sock));
    if (chan == NULL) {
        Ns_TclPrintfResult(interp, "could not open socket");
        ns_sockclose(sock);
        result = TCL_ERROR;
    } else {
        result = Tcl_SetChannelOption(interp, chan, "-translation", "binary");
        if (result == TCL_OK) {
            Tcl_RegisterChannel(interp, chan);
            Tcl_ListObjAppendElement(interp, listObj,
                                     Tcl_NewStringObj(Tcl_GetChannelName(chan), -1));
        }
    }

    return result;
}

static int
EnterDup(Tcl_Interp *interp, NS_SOCKET sock, Tcl_Obj *listObj)
{
    int result;

    NS_NONNULL_ASSERT(interp != NULL);

    sock = ns_sockdup(sock);
    if (sock == NS_INVALID_SOCKET) {
        Ns_TclPrintfResult(interp, "could not dup socket: %s", ns_sockstrerror(errno));
        result = TCL_ERROR;
    } else {
        result = EnterSock(interp, sock, listObj);
    }
    return result;
}

static int
EnterDupedSocks(Tcl_Interp *interp, NS_SOCKET sock, Tcl_Obj *listObj)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);

    if (EnterSock(interp, sock, listObj) != TCL_OK ||
        EnterDup(interp, sock, listObj) != TCL_OK) {
        result = TCL_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockProc --
 *
 *      This is the C wrapper callback that is registered from
 *      callback.
 *
 * Results:
 *      NS_TRUE or NS_FALSE on error
 *
 * Side effects:
 *      Will run Tcl script.
 *
 *----------------------------------------------------------------------
 */

bool
NsTclSockProc(NS_SOCKET sock, void *arg, unsigned int why)
{
    Tcl_DString  script;
    Callback    *cbPtr = arg;

    if (why == (unsigned int)NS_SOCK_EXIT) {
    fail:
        if (cbPtr->chan != NULL) {
            (void) Tcl_UnregisterChannel(NULL, cbPtr->chan);
        } else {
            ns_sockclose(sock);
        }
        ns_free(cbPtr);
        return NS_FALSE;
    }

    if (((cbPtr->when & (unsigned int)NS_SOCK_EXIT) == 0u)) {
        Tcl_Interp  *interp;
        const char  *w;
        int          result;

        Tcl_DStringInit(&script);
        interp = Ns_TclAllocateInterp(cbPtr->server);
        if (cbPtr->chan == NULL) {

            /*
             * Create and register the channel on first use.  Because
             * the Tcl channel code is not entirely thread safe, it is
             * not possible for the scheduling thread to create and
             * register the channel.
             */

            cbPtr->chan = Tcl_MakeTcpClientChannel(INT2PTR(sock));
            if (cbPtr->chan == NULL) {
                Ns_Log(Error, "could not make channel for sock: %d", sock);
                goto fail;
            }
            Tcl_RegisterChannel(NULL, cbPtr->chan);
            (void)Tcl_SetChannelOption(NULL, cbPtr->chan, "-translation", "binary");
        }
        Tcl_RegisterChannel(interp, cbPtr->chan);
        Tcl_DStringAppend(&script, cbPtr->script, -1);
        Tcl_DStringAppendElement(&script, Tcl_GetChannelName(cbPtr->chan));
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
        result = Tcl_EvalEx(interp, script.string, script.length, 0);
        if (result != TCL_OK) {
          (void) Ns_TclLogErrorInfo(interp, "\n(context: sock proc)");
        } else {
            Tcl_Obj *objPtr = Tcl_GetObjResult(interp);
            int      ok = 1;

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

    }


    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SockListenCallback --
 *
 *      This is the C wrapper callback that is registered from
 *      listencallback.
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
    const ListenCallback *lcbPtr = arg;
    Tcl_Interp           *interp;
    Tcl_DString           script;
    Tcl_Obj             **objv;
    int                   result, objc;
    Tcl_Obj             *listObj = Tcl_NewListObj(0, NULL);

    interp = Ns_TclAllocateInterp(lcbPtr->server);
    result = EnterDupedSocks(interp, sock, listObj);

    if (result == TCL_OK) {
        if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK
            && objc == 2) {
            Tcl_DStringInit(&script);
            Tcl_DStringAppend(&script, lcbPtr->script, -1);
            Tcl_DStringAppendElement(&script, Tcl_GetString(objv[0]));
            Tcl_DStringAppendElement(&script, Tcl_GetString(objv[1]));
            result = Tcl_EvalEx(interp, script.string, script.length, 0);
            Tcl_DStringFree(&script);
        }
    }

    if (result != TCL_OK) {
        (void) Ns_TclLogErrorInfo(interp, "\n(context: listen callback)");
    }

    Ns_TclDeAllocateInterp(interp);
    Tcl_DecrRefCount(listObj);

    return NS_TRUE;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
