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
 * NsTclStartContentObjCmd --
 *
 *      Implements ns_startcontent. Set the connection ready to send
 *      body data in an appropriate encoding.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      The connections current encoding may be changed.
 *      See Ns_ConnSetEncoding() for details.
 *
 *----------------------------------------------------------------------
 */

int
NsTclStartContentObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                        Tcl_Obj **objv)
{
    Ns_Conn      *conn;
    Tcl_Encoding  encoding = NULL;
    char         *charset = NULL, *type = NULL;

    Ns_ObjvSpec opts[] = {
        {"-charset", Ns_ObjvString, &charset, NULL},
        {"-type",    Ns_ObjvString, &type, NULL},
        {"--",       Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (GetConn(arg, interp, &conn) != TCL_OK
        || Ns_ParseObjv(opts, NULL, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (charset != NULL && type != NULL) {
        Tcl_SetResult(interp, "only one of -charset or -type may be specified",
                      TCL_STATIC);
        return TCL_ERROR;
    }

    if (charset != NULL) {
        encoding = Ns_GetCharsetEncoding(charset);
        if (encoding == NULL) {
            Tcl_AppendResult(interp, "no encoding for charset: ", charset, NULL);
            return TCL_ERROR;
        }
    }
    if (type != NULL) {
        encoding = Ns_GetTypeEncoding(type);
    }

    if (encoding != NULL) {    
        Ns_ConnSetEncoding(conn, encoding);
    }
    conn->flags |= NS_CONN_SENTHDRS;

    return TCL_OK;
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
    int           length, towrite, nwrote, i, n;
    int           binary = 0;
    struct iovec  iov[32];
    struct iovec *sbufs = iov;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "data ?data ...?");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    objv++;
    objc--;

    /*
     * Allocate space for large numbers of buffers.
     */

    if (objc > sizeof(iov) / sizeof(struct iovec)) {
        sbufs = ns_calloc(objc, sizeof(struct iovec));
    }

    /*
     * If any of the objects are binary, send them all as data without
     * encoding.
     *
     * NB: It's probably a mistake to pass in a mixture of binary and
     * text objects.
     */

    towrite = 0;
    n = 0;
    for (i = 0; i < objc; i++) {
        if (binary || (binary = NsTclObjIsByteArray(objv[i]))) {
            sbufs[n].iov_base = Tcl_GetByteArrayFromObj(objv[i], &length);
        } else {
            sbufs[n].iov_base = Tcl_GetStringFromObj(objv[i], &length);
        }
        if (length > 0) {
            sbufs[n].iov_len = length;
            towrite += length;
            n++;
        }
    }
    if (binary) {
        nwrote = Ns_ConnWriteV(conn, sbufs, n);
    } else {
        nwrote = Ns_ConnWriteVChars(conn, sbufs, n);
    }
    if (sbufs != iov) {
        ns_free(sbufs);
    }

    return Result(interp, (nwrote < towrite) ? NS_ERROR : NS_OK);
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
    if (binary || NsTclObjIsByteArray(dataObj)) {
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

int
NsTclReturnUnavailableObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                             Tcl_Obj *CONST objv[])
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnUnavailable);
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

static int
Result(Tcl_Interp *interp, int result)
{
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(result == NS_OK ? 1 : 0));
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
