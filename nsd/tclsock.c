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
 *	    Tcl commands that let you do TCP socket operation. 
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

static void AppendReadyFiles(Tcl_Interp *interp, const fd_set *setPtr, 
                             int write, const char *flist, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

static int EnterSock(Tcl_Interp *interp, NS_SOCKET sock)
    NS_GNUC_NONNULL(1);
static int EnterDup(Tcl_Interp *interp, NS_SOCKET sock)
    NS_GNUC_NONNULL(1);
static int EnterDupedSocks(Tcl_Interp *interp, NS_SOCKET sock)
    NS_GNUC_NONNULL(1);

static int SockSetBlocking(const char *value, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

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
 *      Performs a reverse DNS lookup. This is the 
 * 
 *      Implementation of "ns_hostbyaddr"
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
NsTclGetHostObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_DString  ds;
    const char *addr;
    int         result = TCL_OK;
    bool        success;

    Ns_ObjvSpec args[] = {
        {"address",  Ns_ObjvString, &addr,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_DStringInit(&ds);
    success = Ns_GetHostByAddr(&ds, addr);

    if (success) {
    	Tcl_DStringResult(interp, &ds);
    } else {
        Tcl_AppendResult(interp, "could not lookup ", addr, NULL);
	result = TCL_ERROR;
    }
    Ns_DStringFree(&ds);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetHostObjCmd --
 *
 *      Performs a DNS lookup.
 *      
 *      Implementation of "ns_addrbyhost".
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
NsTclGetAddrObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_DString  ds;
    const char *host;
    int         all = 0, result = TCL_OK;
    bool        success;
    Ns_ObjvSpec opts[] = {
        {"-all",      Ns_ObjvBool,  &all, INT2PTR(1)},
        {"--",        Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"host",  Ns_ObjvString, &host,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_DStringInit(&ds);
    if (all != 0) {
	success = Ns_GetAllAddrByHost(&ds, host);
    } else {
	success = Ns_GetAddrByHost(&ds, host);
    }
    if (success) {
    	Tcl_DStringResult(interp, &ds);
    } else {
        Tcl_AppendResult(interp, "could not lookup ", host, NULL);
	result = TCL_ERROR;
    }
    Ns_DStringFree(&ds);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockSetBlockingObjCmd --
 *
 *      Sets a socket blocking. 
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
NsTclSockSetBlockingObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return SockSetBlocking("1", interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockSetNonBlockingObjCmd --
 *
 *      Sets a socket nonblocking. 
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
NsTclSockSetNonBlockingObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return SockSetBlocking("0", interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockNReadObjCmd --
 *
 *      Gets the number of bytes that a socket has waiting to be read. 
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
NsTclSockNReadObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    unsigned long nread;
    int           nrBytes;
    Tcl_Channel   chan;
    NS_SOCKET     sock;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        return TCL_ERROR;
    }
    chan = Tcl_GetChannel(interp, Tcl_GetString(objv[1]), NULL);
    if (chan == NULL 
	|| Ns_TclGetOpenFd(interp, Tcl_GetString(objv[1]), 0, (int *) &sock) != TCL_OK) {
        return TCL_ERROR;
    }
    if (ns_sockioctl(sock, FIONREAD, &nread) != 0) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                               "ns_sockioctl failed: ", 
                               Tcl_PosixError(interp), NULL);
        return TCL_ERROR;
    }
    nrBytes = (int)nread;
    nrBytes += Tcl_InputBuffered(chan);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(nrBytes));

    return TCL_OK;
}
    

/*
 *----------------------------------------------------------------------
 *
 * NsTclSockListenObjCmd --
 *
 *      Listen on a TCP port. 
 *
 *      Implementation of "ns_socklisten".
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
NsTclSockListenObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NS_SOCKET   sock;
    const char *addr;
    int         port;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "address port");
        return TCL_ERROR;
    }
    addr = Tcl_GetString(objv[1]);
    if (STREQ(addr, "*")) {
        addr = NULL;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &port) != TCL_OK) {
        return TCL_ERROR;
    }
    sock = Ns_SockListen(addr, port);
    if (sock == NS_INVALID_SOCKET) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), 
                               "could not listen on [\"",
                               Tcl_GetString(objv[1]), "]:", 
                               Tcl_GetString(objv[2]), "\"", NULL);
        return TCL_ERROR;
    }

    return EnterSock(interp, sock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockAcceptObjCmd --
 *
 *      Accept a connection from a listening socket. 
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
NsTclSockAcceptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NS_SOCKET sock;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        return TCL_ERROR;
    }
    if (Ns_TclGetOpenFd(interp, Tcl_GetString(objv[1]), 0, (int *) &sock) != TCL_OK) {
        return TCL_ERROR;
    }
    sock = Ns_SockAccept(sock, NULL, 0);
    if (sock == NS_INVALID_SOCKET) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                               "accept failed: ",
                               Tcl_PosixError(interp), NULL);
        return TCL_ERROR;
    }

    return EnterDupedSocks(interp, sock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockCheckObjCmd --
 *
 *      Check if a socket is still connected, useful for nonblocking. 
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
NsTclSockCheckObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Tcl_Obj   *objPtr;
    NS_SOCKET  sock;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        return TCL_ERROR;
    }
    if (Ns_TclGetOpenFd(interp, Tcl_GetString(objv[1]), 1, (int *) &sock) != TCL_OK) {
        return TCL_ERROR;
    }
    if (ns_send(sock, NULL, 0, 0) != 0) {
        objPtr = Tcl_NewBooleanObj(0);
    } else {
        objPtr = Tcl_NewBooleanObj(1);
    }

    Tcl_SetObjResult(interp, objPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockOpenObjCmd --
 *
 *      Open a tcp connection to a host/port. 
 *
 *      Implementation of "ns_sockopen".
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
NsTclSockOpenObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char    *host, *lhost = NULL;
    int            lport = 0, port, nonblock = 0, async = 0, msec = -1;
    NS_SOCKET      sock;
    Ns_Time        timeout = {0,0};
    const Ns_Time *timeoutPtr = NULL;

    Ns_ObjvSpec opts[] = {
	{"-nonblock",  Ns_ObjvBool,   &nonblock,   INT2PTR(1)},
	{"-async",     Ns_ObjvBool,   &async,      INT2PTR(1)},
        {"-timeout",   Ns_ObjvTime,   &timeoutPtr, NULL},                
	{"-localhost", Ns_ObjvString, &lhost,      NULL},
	{"-localport", Ns_ObjvInt,    &lport,      NULL},
        {"--",         Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"host",      Ns_ObjvString,  &host,       NULL},
        {"port",      Ns_ObjvInt,     &port,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    /*
     * Provide error messages for invalid argument combinations.  Note that either
     *     -nonblock | -async
     * or
     *     -timeout seconds?:microseconds?
     * are accepted as combinations.
     */

    if (nonblock != 0 || async != 0) {
	if (timeoutPtr != NULL) {
	    Ns_TclPrintfResult(interp, "-timeout can't be specified when -async or -nonblock are used");
	    return TCL_ERROR;
	}
	async = 1;
    }
    if (lhost != NULL) {
	if (*lhost == '\0') {
	    Ns_TclPrintfResult(interp, "invalid hostname: must not be empty");
	    return TCL_ERROR;
	}
    }
    if (timeoutPtr != NULL) {
	msec = (int)(timeout.sec * 1000 + timeout.usec / 1000);
    }
    if (lport < 0) {
	Ns_TclPrintfResult(interp, "invalid local port, must be > 0");
	return TCL_ERROR;
    }
    if (*host == '\0') {
	Ns_TclPrintfResult(interp, "invalid hostname: must not be empty");
        return TCL_ERROR;
    }
    if (port < 0) {
	Ns_TclPrintfResult(interp, "invalid port, must be > 0");
	return TCL_ERROR;
    }

    /*
     * Perform the connection.
     */

    if (async != 0) {
        sock = Ns_SockAsyncConnect2(host, port, lhost, lport);
    } else if (msec < 0) {
        sock = Ns_SockConnect2(host, port, lhost, lport);
    } else {
        sock = Ns_SockTimedConnect2(host, port, lhost, lport, timeoutPtr);
    }

    if (sock == NS_INVALID_SOCKET) {
	Ns_TclPrintfResult(interp, "can't connect to \"%s:%d\"; %s",
			   host, port, (Tcl_GetErrno() != 0) ?  Tcl_PosixError(interp) : "reason unknown");
        return TCL_ERROR;
    }
    
    return EnterDupedSocks(interp, sock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSelectObjCmd --
 *
 *      Imlements select: basically a tcl version of select(2).
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
NsTclSelectObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
        tv.tv_sec  = timeout.sec;
        tv.tv_usec = timeout.usec;
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
    if (GetSet(interp, dsRfd.string, 0, &rPtr, &rset, &maxfd) 
        != TCL_OK) {
        goto done;
    }
    if (GetSet(interp, Tcl_GetString(objv[arg++]), 1, &wPtr, &wset, &maxfd)
        != TCL_OK) {
        goto done;
    }
    if (GetSet(interp, Tcl_GetString(objv[arg++]), 0, &ePtr, &eset, &maxfd)
        != TCL_OK) {
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
	NS_SOCKET sock;

        
        do {
            sock = select(maxfd + 1, rPtr, wPtr, ePtr, tvPtr);
        } while (sock == NS_INVALID_SOCKET && errno == EINTR);
        if (sock == NS_INVALID_SOCKET) {
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "select failed: ",
                                   Tcl_PosixError(interp), NULL);
        } else {
            if (sock == 0) {

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
            AppendReadyFiles(interp, rPtr, 0, dsRfd.string, &dsNbuf);
            arg -= 2;
            AppendReadyFiles(interp, wPtr, 1, Tcl_GetString(objv[arg++]), NULL);
            AppendReadyFiles(interp, ePtr, 0, Tcl_GetString(objv[arg++]), NULL);
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
 *      Create a new socket pair. 
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
                      Tcl_Obj *CONST* UNUSED(objv))
{
    NS_SOCKET socks[2];
    
    if (ns_sockpair(socks) != 0) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                               "ns_sockpair failed:  ", 
                               Tcl_PosixError(interp), NULL);
        return TCL_ERROR;
    }
    if (EnterSock(interp, socks[0]) != TCL_OK) {
        ns_sockclose(socks[1]);
        return TCL_ERROR;
    }
    
    return EnterSock(interp, socks[1]);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSockCallbackCmd --
 *
 *      Register a Tcl callback to be run when a certain state exists 
 *      on a socket. 
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
NsTclSockCallbackObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char     *s, *script;
    NS_SOCKET       sock;
    int             scriptLength = 0, result = TCL_OK;
    Ns_Time         timeout;
    const Ns_Time  *timeoutPtr = NULL;
    unsigned int    when;
    Callback       *cbPtr;
    const NsInterp *itPtr = clientData;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId script when ?timeout?");
        return TCL_ERROR;
    }
    s = Tcl_GetString(objv[3]);
    when = 0u;
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
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                                   "invalid when specification \"",
                                   Tcl_GetString(objv[3]), 
                                   "\": should be one/more of r, w, e, or x", 
                                   NULL);
            return TCL_ERROR;
        }
        ++s;
    }
    if (when == 0u) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                               "invalid when specification \"",
                               Tcl_GetString(objv[3]), 
                               "\": should be one/more of r, w, e, or x",
                               NULL);
        return TCL_ERROR;
    }
    if (Ns_TclGetOpenFd(interp, Tcl_GetString(objv[1]),
                        (when & (unsigned int)NS_SOCK_WRITE) != 0u, 
			(int *) &sock) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc > 4) {
        /*
         * timeout was specified, set is just in case the timeout was not 0:0
         */
        if (Ns_TclGetTimeFromObj(interp, objv[4], &timeout) == TCL_OK) {
            if (timeout.sec > 0 || timeout.usec > 0) {
                timeoutPtr = &timeout;
            }
        } else {
            return TCL_ERROR;
        }
    }

    /*
     * Pass a dup of the socket to the callback thread, allowing
     * this thread's cleanup to close the current socket.  It's
     * not possible to simply register the channel again with
     * a NULL interp because the Tcl channel code is not entirely
     * thread safe.
     */

    sock = ns_sockdup(sock);
    script = Tcl_GetStringFromObj(objv[2], &scriptLength);
    
    cbPtr = ns_malloc(sizeof(Callback) + (size_t)scriptLength);
    cbPtr->server = (itPtr->servPtr != NULL ? itPtr->servPtr->server : NULL);
    cbPtr->chan = NULL;
    cbPtr->when = when;
    memcpy(cbPtr->script, script, (size_t)scriptLength + 1u);
    
    if (Ns_SockCallbackEx(sock, NsTclSockProc, cbPtr,
			  when | (unsigned int)NS_SOCK_EXIT, 
			  timeoutPtr, NULL) != NS_OK) {
        Tcl_SetResult(interp, "could not register callback", TCL_STATIC);
        ns_sockclose(sock);
        ns_free(cbPtr);
        result = TCL_ERROR;
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
 *
 *      Implementation of "ns_socklistencallback".
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
NsTclSockListenCallbackObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    ListenCallback *lcbPtr;
    int             port, length;
    const char     *addr, *script;

    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "address port script");
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &port) != TCL_OK) {
        return TCL_ERROR;
    }
    addr = Tcl_GetString(objv[1]);
    if (STREQ(addr, "*")) {
        addr = NULL;
    }
    script = Tcl_GetStringFromObj(objv[3], &length);
    lcbPtr = ns_malloc(sizeof(ListenCallback) + (size_t)length);
    lcbPtr->server = (itPtr->servPtr != NULL ? itPtr->servPtr->server : NULL);
    memcpy(lcbPtr->script, script, (size_t)length + 1u);

    if (Ns_SockListenCallback(addr, port, SockListenCallback, lcbPtr) != NS_OK) {
        Tcl_SetResult(interp, "could not register callback", TCL_STATIC);
        ns_free(lcbPtr);
        return TCL_ERROR;
    }

    return TCL_OK;
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
SockSetBlocking(const char *value, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Tcl_Channel chan;

    NS_NONNULL_ASSERT(value != NULL);
    NS_NONNULL_ASSERT(interp != NULL);

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "sockId");
        return TCL_ERROR;
    }

    chan = Tcl_GetChannel(interp, Tcl_GetString(objv[1]), NULL);

    if (chan == NULL) {
        return TCL_ERROR;
    }

    return Tcl_SetChannelOption(interp, chan, "-blocking", value);
}


/*
 *----------------------------------------------------------------------
 *
 * AppendReadyFiles --
 *
 *      Find files in an fd_set that are selected and append them 
 *      to the tcl result, and also an optional passed-in dstring. 
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Ready files will be appended to pds if not null, and also 
 *      interp result. 
 *
 *----------------------------------------------------------------------
 */

static void
AppendReadyFiles(Tcl_Interp *interp, const fd_set *setPtr, int write, const char *flist,
		 Tcl_DString *dsPtr)
{
    int           fargc = 0;
    const char  **fargv = NULL;
    NS_SOCKET     sock;
    Tcl_DString   ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(flist != NULL);

    Tcl_DStringInit(&ds);
    if (dsPtr == NULL) {
        dsPtr = &ds;
    }
    if (Tcl_SplitList(interp, flist, &fargc, &fargv) == TCL_OK) {
	while (fargc-- > 0) {
	    (void) Ns_TclGetOpenFd(interp, fargv[fargc], write, (int *) &sock);
	    if (FD_ISSET(sock, setPtr)) {
		Tcl_DStringAppendElement(dsPtr, fargv[fargc]);
	    }
	}

	/*
	 * Append the ready files to the tcl interp.
	 */
    
	Tcl_AppendElement(interp, dsPtr->string);
	Tcl_Free((char *) fargv);
    } else {
	Ns_Log(Error, "Can't split list '%s'", flist);
    }
    Tcl_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * GetSet --
 *
 *      Take a Tcl list of files and set bits for each in the list in 
 *      an fd_set. 
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      Will set bits in fd_set. ppset may be NULL on error, or
 *      a valid fd_set on success. Max fd will be returned in *maxPtr.
 *
 *----------------------------------------------------------------------
 */

static int
GetSet(Tcl_Interp *interp, const char *flist, int write, fd_set **setPtrPtr,
       fd_set *setPtr, int *const maxPtr)
{
    int          fargc, status;
    NS_SOCKET    sock;
    const char **fargv = NULL;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(flist != NULL);
    NS_NONNULL_ASSERT(setPtrPtr != NULL);
    NS_NONNULL_ASSERT(setPtr != NULL);
    NS_NONNULL_ASSERT(maxPtr != NULL);
    
    if (Tcl_SplitList(interp, flist, &fargc, &fargv) != TCL_OK) {
        return TCL_ERROR;
    }
    if (fargc == 0) {
        ckfree((char *)fargv);
        *setPtrPtr = NULL;
        return TCL_OK;
    } else {
        *setPtrPtr = setPtr;
    }
    
    FD_ZERO(setPtr);
    status = TCL_OK;

    /*
     * Loop over each file, try to get its FD, and set the bit in
     * the fd_set.
     */
    
    while (fargc-- > 0) {
        if (Ns_TclGetOpenFd(interp, fargv[fargc],
                            write, (int *) &sock) != TCL_OK) {
            status = TCL_ERROR;
            break;
        }
#ifndef _MSC_VER
	/* winsock ignores first argument of select */
        if (sock > *maxPtr) {
            *maxPtr = sock;
        }
#endif
        FD_SET(sock, setPtr);
    }
    Tcl_Free((char *) fargv);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * EnterSock, EnterDup, EnterDupedSocks --
 *
 *      Append a socket handle to the tcl result and register its 
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
EnterSock(Tcl_Interp *interp, NS_SOCKET sock)
{
    Tcl_Channel chan;
    int result;

    NS_NONNULL_ASSERT(interp != NULL);

    chan = Tcl_MakeTcpClientChannel(INT2PTR(sock));
    if (chan == NULL) {
        Tcl_AppendResult(interp, "could not open socket", NULL);
        ns_sockclose(sock);
        return TCL_ERROR;
    }
    result = Tcl_SetChannelOption(interp, chan, "-translation", "binary");
    if (result == TCL_OK) {
	Tcl_RegisterChannel(interp, chan);
	Tcl_AppendElement(interp, Tcl_GetChannelName(chan));
    }

    return result;
}

static int
EnterDup(Tcl_Interp *interp, NS_SOCKET sock)
{
    NS_NONNULL_ASSERT(interp != NULL);

    sock = ns_sockdup(sock);
    if (sock == NS_INVALID_SOCKET) {
        Tcl_AppendResult(interp, "could not dup socket: ", 
                         ns_sockstrerror(errno), NULL);
        return TCL_ERROR;
    }

    return EnterSock(interp, sock);
}

static int
EnterDupedSocks(Tcl_Interp *interp, NS_SOCKET sock)
{
    NS_NONNULL_ASSERT(interp != NULL);

    if (EnterSock(interp, sock) != TCL_OK ||
        EnterDup(interp, sock) != TCL_OK) {
        return TCL_ERROR;
    }

    return TCL_OK;
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
             * the Tcl channel code is not entirely thread safe, it's
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

    interp = Ns_TclAllocateInterp(lcbPtr->server);
    result = EnterDupedSocks(interp, sock);

    if (result == TCL_OK) {
        Tcl_Obj  *listPtr = Tcl_GetObjResult(interp);

        if (Tcl_ListObjGetElements(interp, listPtr, &objc, &objv) == TCL_OK 
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

    return NS_TRUE;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
