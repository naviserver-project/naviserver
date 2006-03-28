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
 * tclresp.c --
 *
 *      Tcl commands for returning data to the user agent. 
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


/*
 * Static functions defined in this file.
 */

static int WritevObjs(Tcl_Interp *interp, Ns_Conn *conn, int objc,
                      Tcl_Obj *CONST objv[]);
static int Result(Tcl_Interp *interp, int result);
static int GetConn(ClientData arg, Tcl_Interp *interp, Ns_Conn **connPtr);



/*
 *----------------------------------------------------------------------
 *
 * NsTclHeadersObjCmd --
 *
 *      Implements ns_headers. Queue default response headers, which
 *      will be sent on the first IO (e.g. ns_write) or otherwise when
 *      the connection is closed.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 (success) always.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclHeadersObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                   Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;
    int      status, len = -1;
    char    *type = NULL;

    if (objc < 2 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "status ?type? ?len?");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &status) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc > 2) {
        type = Tcl_GetString(objv[2]);
    }
    if (objc > 3 && Tcl_GetIntFromObj(interp, objv[3], &len) != TCL_OK) {
        return TCL_ERROR;
    }

    Ns_ConnSetRequiredHeaders(conn, type, len);
    Ns_ConnQueueHeaders(conn, status);

    return Result(interp, NS_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclWriteObjCmd --
 *
 *      Implements ns_write. Send data directly to client without
 *      buffering.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      String may be transcoded. Data may be sent HTTP chunked.
 *
 *----------------------------------------------------------------------
 */

int
NsTclWriteObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                 Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "data ?data ...?");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    return WritevObjs(interp, conn, objc - 1, objv + 1);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReturnObjCmd --
 *
 *      Implements ns_return.  Send complete response to client with
 *      given data as body.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      String may be transcoded if binary switch not given. Connection
 *      will be closed.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                  Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;
    Tcl_Obj *dataObj;
    char    *type, *data;
    int      status, len, result, binary = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-binary",  Ns_ObjvBool, &binary, (void *) NS_TRUE},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"status",   Ns_ObjvInt,    &status,  NULL},
        {"type",     Ns_ObjvString, &type,    NULL},
        {"data",     Ns_ObjvObj,    &dataObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (binary) {
        data = (char *) Tcl_GetByteArrayFromObj(dataObj, &len);
        result = Ns_ConnReturnData(conn, status, data, len, type);
    } else {
        data = Tcl_GetStringFromObj(dataObj, &len);
        result = Ns_ConnReturnCharData(conn, status, data, len, type);
    }

    return Result(interp, result);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRespondObjCmd --
 *
 *      Implements ns_respond.  Send complete response to client using
 *      a variety of options.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      String data may be transcoded. Connection will be closed.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRespondObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                   Tcl_Obj *CONST objv[])
{
    Ns_Conn     *conn;
    int          status = 200, length = -1;
    char        *type = "*/*", *setid = NULL, *binary = NULL;
    char        *string = NULL, *filename = NULL, *chanid = NULL;
    Ns_Set      *set = NULL;
    Tcl_Channel  chan;
    int          result;

    Ns_ObjvSpec opts[] = {
        {"-status",   Ns_ObjvInt,       &status,   NULL},
        {"-type",     Ns_ObjvString,    &type,     NULL},
        {"-length",   Ns_ObjvInt,       &length,   NULL},
        {"-headers",  Ns_ObjvString,    &setid,    NULL},
        {"-string",   Ns_ObjvString,    &string,   NULL},
        {"-file",     Ns_ObjvString,    &filename, NULL},
        {"-fileid",   Ns_ObjvString,    &chanid,   NULL},
        {"-binary",   Ns_ObjvByteArray, &binary,   &length},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, NULL, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (chanid != NULL && length < 0) {
        Tcl_SetResult(interp, "length required when -fileid is used", 
                      TCL_STATIC);
        return TCL_ERROR;
    }
    if ((binary != NULL) + (string != NULL) + (filename != NULL)
        + (chanid != NULL) != 1) {
        Tcl_SetResult(interp, "must specify only one of -string, "
                      "-file, -binary or -fileid", TCL_STATIC);
        return TCL_ERROR;
    }
    if (setid != NULL) {
        set = Ns_TclGetSet(interp, setid);
        if (set == NULL) {
            Ns_TclPrintfResult(interp, "illegal ns_set id: \"%s\"", setid);
            return TCL_ERROR;
        }
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (set != NULL) {
        Ns_ConnReplaceHeaders(conn, set);
    }

    if (chanid != NULL) {
        /*
         * We'll be returning an open channel
         */

        if (Ns_TclGetOpenChannel(interp, chanid, 0, 1, &chan) != TCL_OK) {
            return TCL_ERROR;
        }
        result = Ns_ConnReturnOpenChannel(conn, status, type, chan, length);

    } else if (filename != NULL) {
        /*
         * We'll be returning a file by name
         */

        result = Ns_ConnReturnFile(conn, status, type, filename);

    } else if (binary != NULL) {
        /*
         * We'll be returning a binary data
         */

        result = Ns_ConnReturnData(conn, status, binary, length, type);

    } else {
        /*
         * We'll be returning a string now.
         */

        result = Ns_ConnReturnCharData(conn, status, string, length, type);
    }

    return Result(interp, result);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReturnFileObjCmd --
 *
 *      Implements ns_returnfile.  Send complete response to client
 *      using contents of filename if exists and is readable, otherwise
 *      send error response.
 *
 * Results:
 *      0 - failure, 1 - success
 *      Tcl error on wrong syntax/arguments
 *
 * Side effects:
 *      Fastpath cache may be used. Connection will be closed.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnFileObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                      Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;
    int      status, result;

    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "status type filename");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &status) != TCL_OK) {
        return TCL_ERROR;
    }
    
    result = Ns_ConnReturnFile(conn, status, Tcl_GetString(objv[2]), 
                               Tcl_GetString(objv[3]));

    return Result(interp, result);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReturnFpObjCmd --
 *
 *      Implements ns_returnfp.  Send complete response to client using
 *      len bytes from given channel.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnFpObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                    Tcl_Obj *CONST objv[])
{
    int          len, status, result;
    Tcl_Channel  chan;
    Ns_Conn     *conn;

    if (objc != 5) {
        Tcl_WrongNumArgs(interp, 1, objv, "status type channel len");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &status) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[4], &len) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Ns_TclGetOpenChannel(interp, Tcl_GetString(objv[3]), 0, 1, &chan)
        != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnReturnOpenChannel(conn, status, Tcl_GetString(objv[2]),
                                      chan, len);

    return Result(interp, result);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConnSendFpObjCmd --
 *
 *      Implements ns_connsendfp.  Send len bytes from given channel
 *      directly to client without sending headers.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclConnSendFpObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                      Tcl_Obj *CONST objv[])
{
    Ns_Conn     *conn;
    Tcl_Channel  chan;
    int          len;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "channel len");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Ns_TclGetOpenChannel(interp, Tcl_GetString(objv[1]), 0, 1, &chan) 
        != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &len) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Ns_ConnSendChannel(conn, chan, len) != NS_OK) {
        Ns_TclPrintfResult(interp, "could not send %d bytes from channel %s",
                           len, Tcl_GetString(objv[1]));
        return TCL_ERROR;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReturnBadRequestObjCmd --
 *
 *      Implements ns_returnbadrequest.  Send an error response to
 *      client with HTTP status code 400.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnBadRequestObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                            Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;
    int      result;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "reason");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnReturnBadRequest(conn, Tcl_GetString(objv[1]));

    return Result(interp, result);
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnObjCmd --
 *
 *      Implements ns_returnnotfound, ns_returnunauthorized and
 *      ns_returnforbidden.  Send an error response to client.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

static int
ReturnObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
        Tcl_Obj *CONST objv[], int (*proc) (Ns_Conn *))
{
    Ns_Conn *conn;

    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    return Result(interp, (*proc)(conn));
}

int
NsTclReturnNotFoundObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                          Tcl_Obj *CONST objv[])
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnNotFound);
}

int
NsTclReturnUnauthorizedObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                              Tcl_Obj *CONST objv[])
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnUnauthorized);
}

int
NsTclReturnForbiddenObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                           Tcl_Obj *CONST objv[])
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnForbidden);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReturnErrorObjCmd --
 *
 *      Implements ns_returnerror.  Send an error response to client
 *      with given status code and message.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnErrorObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                       Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;
    int      status, result;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "status message");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &status) != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnReturnNotice(conn, status, "Request Error",
                                 Tcl_GetString(objv[2]));

    return Result(interp, result);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReturnNoticeObjCmd --
 *
 *      Implements ns_returnnotice command.  Send a response to client
 *      with given status code, title and message.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnNoticeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                        Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;
    int      status, result;

    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "status title message");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &status) != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnReturnNotice(conn, status, Tcl_GetString(objv[2]),
                                 Tcl_GetString(objv[3]));

    return Result(interp, result);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReturnRedirectObjCmd --
 *
 *      Implements ns_returnredirect.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      See Ns_ConnReturnRedirect().
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnRedirectObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                          Tcl_Obj *CONST objv[])
{
    Ns_Conn *conn;
    int      result;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "location");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnReturnRedirect(conn, Tcl_GetString(objv[1]));

    return Result(interp, result);
}



/*
 *----------------------------------------------------------------------
 *
 * WritevObjs --
 *
 *      Write the given Tcl objects directly to the client using
 *      vectored IO.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      String may be encoded if the WriteEncoded flag of the conn
 *      is set.
 *
 *----------------------------------------------------------------------
 */

static int
WritevObjs(Tcl_Interp *interp, Ns_Conn *conn, int objc, Tcl_Obj *CONST objv[])
{
    int           length, towrite, nwrote, i;
    int           binary = 0;
    struct iovec  iov[32];
    struct iovec *sbufs = iov;

    /*
     * Treat string as binary unless the WriteEncodedFlag is set
     * on the current conn.  This flag is manipulated via
     * ns_startcontent or ns_conn write_encoded.
     */

    if (!Ns_ConnGetWriteEncodedFlag(conn)) {
        binary = 1;
    }
    if (objc > sizeof(iov) / sizeof(struct iovec)) {
        sbufs = ns_calloc(objc, sizeof(struct iovec));
    }
    towrite = 0;
    for (i = 0; i < objc; i++) {
        if (binary) {
            sbufs[i].iov_base = Tcl_GetByteArrayFromObj(objv[i], &length);
        } else {
            sbufs[i].iov_base = Tcl_GetStringFromObj(objv[i], &length);
        }
        sbufs[i].iov_len = length;
        towrite += length;
    };
    if (binary) {
        nwrote = Ns_ConnWriteV(conn, sbufs, objc);
    } else {
        nwrote = Ns_ConnWriteVChars(conn, sbufs, objc);
    }
    if (sbufs != iov) {
        ns_free(sbufs);
    }

    return Result(interp, (nwrote < towrite) ? NS_ERROR : NS_OK);
}

static int
Result(Tcl_Interp *interp, int result)
{
    Tcl_SetBooleanObj(Tcl_GetObjResult(interp), result == NS_OK ? 1 : 0);
    return TCL_OK;
}


static int
GetConn(ClientData arg, Tcl_Interp *interp, Ns_Conn **connPtr)
{
    NsInterp *itPtr = arg;

    if (itPtr->conn == NULL) {
        Tcl_SetResult(interp, "no connection", TCL_STATIC);
        return TCL_ERROR;
    }

    *connPtr = itPtr->conn;

    return TCL_OK;
}
