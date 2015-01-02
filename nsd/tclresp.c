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

/*
 * Static functions defined in this file.
 */

static int Result(Tcl_Interp *interp, int result) NS_GNUC_NONNULL(1);

static int GetConn(ClientData arg, Tcl_Interp *interp, Ns_Conn **connPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int ReturnObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, int (*proc) (Ns_Conn *conn))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);




/*
 *----------------------------------------------------------------------
 *
 * NsTclHeadersObjCmd --
 *
 *      Implements ns_headers. Set the response status code, mime-type
 *      header and optionaly the content-length. The headers will be
 *      be written on the first write to the connection (if not suppressed).
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 (success) always.
 *
 * Side effects:
 *      May change the connections output encoding/append charset to
 *      given mime-type.
 *
 *----------------------------------------------------------------------
 */

int
NsTclHeadersObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp   *itPtr = arg;
    Ns_Conn    *conn = NULL;
    int         status, length = -1, binary = 0;
    const char *type = NULL;

    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool,  &binary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"status",  Ns_ObjvInt,    &status, NULL},
        {"?type",   Ns_ObjvString, &type, NULL},
        {"?length", Ns_ObjvInt,    &length, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK
            || GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    Ns_ConnSetResponseStatus(conn, status);

    if (type != NULL) {
        if (binary != 0) {
            Ns_ConnSetTypeHeader(conn, type);
        } else {
            Ns_ConnSetEncodedTypeHeader(conn, type);
        }
    } else if (binary != 0) {
        conn->flags |= NS_CONN_WRITE_ENCODED;
    }

    if (length > -1) {
	Ns_ConnSetLengthHeader(conn, (size_t)length, 0);
    }

    /*
     * Request HTTP headers from ns_write etc.
     */
    itPtr->nsconn.flags |= CONN_TCLHTTP;

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
 *      Deprecated.
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
NsTclStartContentObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp     *itPtr = arg;
    Ns_Conn      *conn = NULL;
    Tcl_Encoding  encoding = NULL;
    const char   *charset = NULL, *type = NULL;

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

    itPtr->nsconn.flags |= CONN_TCLHTTP;

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
NsTclWriteObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp     *itPtr = arg;
    Ns_Conn      *conn  = NULL;
    int           length, i, n, binary, status;
    unsigned int  flags;
    struct iovec  iov[32];
    struct iovec *sbufs = iov;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "data ?data ...?");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Ns_ConnSockPtr(conn) == NULL) {
        Ns_TclPrintfResult(interp, "connection channels is detached");
        return TCL_ERROR;
    }
    objv++;
    objc--;

    /*
     * On first write, check to see if headers were requested by ns_headers.
     * Otherwise, supress them -- caller will ns_write the headers
     * or this is some other protocol.
     */

    if ((conn->flags & NS_CONN_SENTHDRS) == 0u
	&& (itPtr->nsconn.flags & CONN_TCLHTTP) == 0u) {
        conn->flags |= NS_CONN_SKIPHDRS;
    }

    /*
     * Allocate space for large numbers of buffers.
     */

    if (objc > (int)(sizeof(iov) / sizeof(struct iovec))) {
        sbufs = ns_calloc((size_t)objc, sizeof(struct iovec));
    }

    /*
     * If the -binary switch was given to ns_headers, treat all
     * objects as binary data.
     *
     * If any of the objects are binary, send them all as data without
     * encoding.
     *
     * NB: It's probably a mistake to pass in a mixture of binary and
     * text objects...
     */

    binary = (conn->flags & NS_CONN_WRITE_ENCODED) != 0u ? NS_FALSE : NS_TRUE;

    for (i = 0, n = 0; i < objc; i++) {
	if (binary == NS_FALSE) {
	    binary = NsTclObjIsByteArray(objv[i]);
	}
	if (binary == NS_TRUE) {
	    sbufs[n].iov_base = (void *)Tcl_GetByteArrayFromObj(objv[i], &length);
        } else {
            sbufs[n].iov_base = Tcl_GetStringFromObj(objv[i], &length);
        }
        if (length > 0) {
            sbufs[n].iov_len = (size_t)length;
            n++;
        }
    }

    /*
     * Don't stream if the user has explicitly set the content-length,
     * as chunking would alter this.
     */

    flags = 0U;
    if (Ns_ConnResponseLength(conn) < 0) {
        flags |= NS_CONN_STREAM;
    }

    if (binary != 0) {
        status = Ns_ConnWriteVData(conn, sbufs, n, flags);
    } else {
        status = Ns_ConnWriteVChars(conn, sbufs, n, flags);
    }
    if (sbufs != iov) {
        ns_free(sbufs);
    }

    return Result(interp, status);
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
NsTclReturnObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn    *conn = NULL;
    Tcl_Obj    *dataObj;
    const char *type, *data;
    int         status, len, result, binary = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-binary",  Ns_ObjvBool, &binary, INT2PTR(NS_TRUE)},
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
    if (binary == NS_TRUE || NsTclObjIsByteArray(dataObj) == NS_TRUE) {
        data = (const char *) Tcl_GetByteArrayFromObj(dataObj, &len);
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
NsTclRespondObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn     *conn = NULL;
    int          result, status = 200, length = -1;
    const char  *type = "*/*", *setid = NULL, *binary = NULL;
    const char  *chars = NULL, *filename = NULL, *chanid = NULL;
    Ns_Set      *set = NULL;
    Tcl_Channel  chan;

    Ns_ObjvSpec opts[] = {
        {"-status",   Ns_ObjvInt,       &status,   NULL},
        {"-type",     Ns_ObjvString,    &type,     NULL},
        {"-length",   Ns_ObjvInt,       &length,   NULL},
        {"-headers",  Ns_ObjvString,    &setid,    NULL},
        {"-string",   Ns_ObjvString,    &chars,    NULL},
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
    if ((binary != NULL) + (chars != NULL) + (filename != NULL)
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
        result = Ns_ConnReturnOpenChannel(conn, status, type, chan, (size_t)length);

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
         * We'll be returning chars.
         */

        result = Ns_ConnReturnCharData(conn, status, chars, length, type);
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
NsTclReturnFileObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn *conn = NULL;
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
NsTclReturnFpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          len, status, result;
    Tcl_Channel  chan;
    Ns_Conn     *conn = NULL;

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
    if (Ns_TclGetOpenChannel(interp, Tcl_GetString(objv[3]), 0, 1, &chan) != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnReturnOpenChannel(conn, status, Tcl_GetString(objv[2]),
                                      chan, (size_t)len);

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
NsTclConnSendFpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn     *conn = NULL;
    Tcl_Channel  chan;
    int          len;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "channel len");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Ns_TclGetOpenChannel(interp, Tcl_GetString(objv[1]), 0, 1, &chan) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &len) != TCL_OK) {
        return TCL_ERROR;
    }

    Ns_LogDeprecated(objv, 3, "ns_writefp fileid ?nbytes?", NULL);

    conn->flags |= NS_CONN_SKIPHDRS;
    if (Ns_ConnSendChannel(conn, chan, (size_t)len) != NS_OK) {
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
NsTclReturnBadRequestObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn *conn = NULL;
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
ReturnObjCmd(ClientData arg, Tcl_Interp *interp, 
	     int UNUSED(objc), Tcl_Obj *CONST* UNUSED(objv), 
	     int (*proc) (Ns_Conn *conn))
{
    Ns_Conn *conn = NULL;

    assert(arg != NULL);
    assert(interp != NULL);

    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    return Result(interp, (*proc)(conn));
}

int
NsTclReturnNotFoundObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnNotFound);
}

int
NsTclReturnUnauthorizedObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnUnauthorized);
}

int
NsTclReturnForbiddenObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnForbidden);
}

int
NsTclReturnUnavailableObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnUnavailable);
}

int
NsTclReturnTooLargeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(arg, interp, objc, objv, Ns_ConnReturnEntityTooLarge);
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
NsTclReturnErrorObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn *conn = NULL;
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
 * NsTclReturnMovedObjCmd --
 *
 *      Implements ns_returnmoved.
 *
 * Results:
 *      Standard Tcl result.
 *      Interpreter result set to 0 on success or 1 on failure.
 *
 * Side effects:
 *      See Ns_ConnReturnMoved().
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnMovedObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn *conn = NULL;
    int      result;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "location");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnReturnMoved(conn, Tcl_GetString(objv[1]));

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
NsTclReturnNoticeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn *conn = NULL;
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
NsTclReturnRedirectObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn *conn = NULL;
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
 * NsTclInternalRedirectObjCmd --
 *
 *	Implements ns_internalredirect as obj command.
 *
 * Results:
 *	Tcl result.
 *
 * Side effects:
 *	See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclInternalRedirectObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn *conn = NULL;
    int      result;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "location");
        return TCL_ERROR;
    }
    if (GetConn(arg, interp, &conn) != TCL_OK) {
        return TCL_ERROR;
    }

    result = Ns_ConnRedirect(conn, Tcl_GetString(objv[1]));

    return Result(interp, result);
}

static int
Result(Tcl_Interp *interp, int result)
{
    assert(interp != NULL);
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(result == NS_OK ? 1 : 0));
    return TCL_OK;
}


static int
GetConn(ClientData arg, Tcl_Interp *interp, Ns_Conn **connPtr)
{
    NsInterp *itPtr = arg;

    assert(arg != NULL);
    assert(interp != NULL);
    assert(connPtr != NULL);

    if (itPtr->conn == NULL) {
        Tcl_SetResult(interp, "no connection", TCL_STATIC);
        return TCL_ERROR;
    }

    *connPtr = itPtr->conn;

    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
