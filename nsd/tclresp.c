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

static int Result(Tcl_Interp *interp, Ns_ReturnCode result)
    NS_GNUC_NONNULL(1);

static int ReturnObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv,
                        Ns_ReturnCode (*proc) (Ns_Conn *conn))
    NS_GNUC_NONNULL(2);




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
NsTclHeadersObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp   *itPtr = clientData;
    Ns_Conn    *conn = NULL;
    int         httpStatus, length = -1, binary = 0, result;
    char       *mimeType = NULL;

    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool,  &binary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"status",  Ns_ObjvInt,    &httpStatus, NULL},
        {"?type",   Ns_ObjvString, &mimeType, NULL},
        {"?length", Ns_ObjvInt,    &length, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;
    } else {

        Ns_ConnSetResponseStatus(conn, httpStatus);

        if (mimeType != NULL) {
            if (binary != 0) {
                Ns_ConnSetTypeHeader(conn, mimeType);
            } else {
                Ns_ConnSetEncodedTypeHeader(conn, mimeType);
            }
        } else if (binary != 0) {
            conn->flags |= NS_CONN_WRITE_ENCODED;
        }
        
        if (length > -1) {
            Ns_ConnSetLengthHeader(conn, (size_t)length, NS_FALSE);
        }
        
        /*
         * Request HTTP headers from ns_write etc.
         */
        itPtr->nsconn.flags |= CONN_TCLHTTP;
        result = Result(interp, NS_OK);
    }

    return result;
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
NsTclStartContentObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp     *itPtr = clientData;
    Ns_Conn      *conn = NULL;
    Tcl_Encoding  encoding = NULL;
    int           result = TCL_OK;
    char         *charset = NULL, *type = NULL;

    Ns_ObjvSpec opts[] = {
        {"-charset", Ns_ObjvString, &charset, NULL},
        {"-type",    Ns_ObjvString, &type, NULL},
        {"--",       Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, NULL, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;

    } else if (charset != NULL && type != NULL) {
        Ns_TclPrintfResult(interp, "only one of -charset or -type may be specified");
        result = TCL_ERROR;
        
    } else {
        
        Ns_LogDeprecated(objv, 1, "ns_headers ...", NULL);

        itPtr->nsconn.flags |= CONN_TCLHTTP;

        if (charset != NULL) {
            encoding = Ns_GetCharsetEncoding(charset);
            if (encoding == NULL) {
                Ns_TclPrintfResult(interp, "no encoding for charset: %s", charset);
                result = TCL_ERROR;
            }
        }
        if (result == TCL_OK) {
            
            if (type != NULL) {
                encoding = Ns_GetTypeEncoding(type);
            }
            
            if (encoding != NULL) {
                Ns_ConnSetEncoding(conn, encoding);
            }
            conn->flags |= NS_CONN_SENTHDRS;
        }
    }
    return result;
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
NsTclWriteObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    Ns_Conn        *conn  = NULL;
    int             length, i, n, result;
    Ns_ReturnCode   status;
    bool            binary;
    unsigned int    flags;
    struct iovec    iov[32];
    struct iovec   *sbufs = iov;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "data ?data ...?");
        result = TCL_ERROR;
        
    } else if (NsConnRequire(interp, &conn) != NS_OK) {
        result = TCL_ERROR;
        
    } else if (Ns_ConnSockPtr(conn) == NULL) {
        Ns_TclPrintfResult(interp, "connection channels is detached");
        result = TCL_ERROR;

    } else {
        objv++;
        objc--;

        /*
         * On first write, check to see if headers were requested by ns_headers.
         * Otherwise, suppress them -- caller will ns_write the headers
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
            if (!binary) {
                binary = NsTclObjIsByteArray(objv[i]);
            }
            if (binary) {
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

        flags = 0u;
        if (Ns_ConnResponseLength(conn) < 0) {
            flags |= NS_CONN_STREAM;
        }

        if (binary) {
            status = Ns_ConnWriteVData(conn, sbufs, n, flags);
        } else {
            status = Ns_ConnWriteVChars(conn, sbufs, n, flags);
        }
        if (sbufs != iov) {
            ns_free(sbufs);
        }

        result = Result(interp, status);
    }

    return result;
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
NsTclReturnObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn    *conn = NULL;
    Tcl_Obj    *dataObj;
    char       *type;
    int         result, httpStatus, len, binary = (int)NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-binary",  Ns_ObjvBool, &binary, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"status",   Ns_ObjvInt,    &httpStatus,  NULL},
        {"type",     Ns_ObjvString, &type,    NULL},
        {"data",     Ns_ObjvObj,    &dataObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;
        
    } else {
        const char *data;

        if (binary || NsTclObjIsByteArray(dataObj)) {
            data = (const char *) Tcl_GetByteArrayFromObj(dataObj, &len);
            result = Result(interp, Ns_ConnReturnData(conn, httpStatus, data, len, type));
        } else {
            data = Tcl_GetStringFromObj(dataObj, &len);
            result = Result(interp, Ns_ConnReturnCharData(conn, httpStatus, data, len, type));
        }
    }

    return  result;
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
NsTclRespondObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn       *conn = NULL;
    int            result = TCL_OK, httpStatus = 200, length = -1;
    char          *type = (char *)"*/*", *setid = NULL;
    char          *chars = NULL, *filename = NULL, *chanid = NULL, *binary = NULL;
    const Ns_Set  *set = NULL;
    Tcl_Channel    chan;

    Ns_ObjvSpec opts[] = {
        {"-status",   Ns_ObjvInt,       &httpStatus, NULL},
        {"-type",     Ns_ObjvString,    &type,       NULL},
        {"-length",   Ns_ObjvInt,       &length,     NULL},
        {"-headers",  Ns_ObjvString,    &setid,      NULL},
        {"-string",   Ns_ObjvString,    &chars,      NULL},
        {"-file",     Ns_ObjvString,    &filename,   NULL},
        {"-fileid",   Ns_ObjvString,    &chanid,     NULL},
        {"-binary",   Ns_ObjvByteArray, &binary,    &length},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(opts, NULL, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;

    } else if (chanid != NULL && length < 0) {
        Ns_TclPrintfResult(interp, "length required when -fileid is used");
        result = TCL_ERROR;

    } else if ((binary != NULL)
        + (chars != NULL)
        + (filename != NULL)
        + (chanid != NULL) != 1
        ) {
        Ns_TclPrintfResult(interp, "must specify only one of -string, "
                           "-file, -binary or -fileid");
        result = TCL_ERROR;

    } else if (setid != NULL) {
        set = Ns_TclGetSet(interp, setid);
        if (set == NULL) {
            Ns_TclPrintfResult(interp, "invalid ns_set id: \"%s\"", setid);
            result = TCL_ERROR;
        }
    }
    if (result == TCL_OK) {
        Ns_ReturnCode  status;

        if (set != NULL) {
            Ns_ConnReplaceHeaders(conn, set);
        }

        if (chanid != NULL) {
            /*
             * We'll be returning an open channel
             */
            if (Ns_TclGetOpenChannel(interp, chanid, 0, NS_TRUE, &chan) != TCL_OK) {
                status = NS_ERROR;
            } else {
                status = Ns_ConnReturnOpenChannel(conn, httpStatus, type, chan, (size_t)length);
            }
            
        } else if (filename != NULL) {
            /*
             * We'll be returning a file by name
             */
            status = Ns_ConnReturnFile(conn, httpStatus, type, filename);
            
        } else if (binary != NULL) {
            /*
             * We'll be returning a binary data
             */
            status = Ns_ConnReturnData(conn, httpStatus, binary, length, type);
            
        } else {
            /*
             * We'll be returning chars.
             */
            status = Ns_ConnReturnCharData(conn, httpStatus, chars, length, type);
        }
        
        result = Result(interp, status);
    }
    return result;
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
 *      Tcl result code
 *      Tcl error on wrong syntax/arguments
 *
 * Side effects:
 *      Fastpath cache may be used. Connection will be closed.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnFileObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn      *conn = NULL;
    int           httpStatus, result;
    char         *mimeType, *fileName;
    Ns_ObjvSpec   args[] = {
        {"status",   Ns_ObjvInt,    &httpStatus,   NULL},
        {"type",     Ns_ObjvString, &mimeType, NULL},
        {"filename", Ns_ObjvString, &fileName, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;

    } else {
        result = Result(interp, Ns_ConnReturnFile(conn, httpStatus, mimeType, fileName));
    }
    
    return result;
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
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnFpObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int           len, httpStatus, result;
    char         *mimeType, *channelName;
    Ns_Conn      *conn = NULL;
    Tcl_Channel   chan = NULL;
    Ns_ObjvSpec   args[] = {
        {"status",  Ns_ObjvInt,    &httpStatus, NULL},
        {"type",    Ns_ObjvString, &mimeType, NULL},
        {"channel", Ns_ObjvString, &channelName, NULL},
        {"len",     Ns_ObjvInt,    &len, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;

    } else {
        result = Ns_TclGetOpenChannel(interp, channelName, 0, NS_TRUE, &chan);
        if (likely( result == TCL_OK )) {
            result = Result(interp, Ns_ConnReturnOpenChannel(conn, httpStatus, mimeType,
                                                             chan, (size_t)len));
        }
    }
    return result;
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
NsTclConnSendFpObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn     *conn = NULL;
    Tcl_Channel  chan = NULL;
    int          len, result = TCL_OK;
    char        *channelName;
    Ns_ObjvSpec  args[] = {
        {"channel", Ns_ObjvString,  &channelName, NULL},
        {"len",     Ns_ObjvInt,     &len,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;

    } else {
        result = Ns_TclGetOpenChannel(interp, channelName, 0, NS_TRUE, &chan);
        if (likely( result == TCL_OK )) {
            Ns_ReturnCode status;
            
            Ns_LogDeprecated(objv, 3, "ns_writefp fileid ?nbytes?", NULL);
            
            conn->flags |= NS_CONN_SKIPHDRS;
            status = Ns_ConnSendChannel(conn, chan, (size_t)len);
            
            if (status != NS_OK) {
                Ns_TclPrintfResult(interp, "could not send %d bytes from channel %s",
                                   len, channelName);
                result = TCL_ERROR;
            }
        }
    }

    return result;
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
NsTclReturnBadRequestObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn      *conn = NULL;
    int           result;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "reason");
        result = TCL_ERROR;
        
    } else if (NsConnRequire(interp, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else {
        result = Result(interp, Ns_ConnReturnBadRequest(conn, Tcl_GetString(objv[1])));
    }
    
    return result;
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
ReturnObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, 
	     int UNUSED(objc), Tcl_Obj *CONST* UNUSED(objv), 
	     Ns_ReturnCode (*proc) (Ns_Conn *conn))
{
    Ns_Conn *conn = NULL;
    int      result;

    NS_NONNULL_ASSERT(interp != NULL);

    if (NsConnRequire(interp, &conn) != NS_OK) {
        result = TCL_ERROR;
    } else {
        result = Result(interp, (*proc)(conn));
    }
    return result;
}

int
NsTclReturnNotFoundObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(clientData, interp, objc, objv, Ns_ConnReturnNotFound);
}

int
NsTclReturnUnauthorizedObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(clientData, interp, objc, objv, Ns_ConnReturnUnauthorized);
}

int
NsTclReturnForbiddenObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(clientData, interp, objc, objv, Ns_ConnReturnForbidden);
}

int
NsTclReturnUnavailableObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(clientData, interp, objc, objv, Ns_ConnReturnUnavailable);
}

int
NsTclReturnTooLargeObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return ReturnObjCmd(clientData, interp, objc, objv, Ns_ConnReturnEntityTooLarge);
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
NsTclReturnErrorObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          httpStatus, result;
    Ns_Conn     *conn = NULL;    
    char        *message;
    Ns_ObjvSpec  args[] = {
        {"status",   Ns_ObjvInt,    &httpStatus,   NULL},
        {"message",  Ns_ObjvString, &message, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;
    } else {
        result = Result(interp, Ns_ConnReturnNotice(conn, httpStatus, "Request Error", message));
    }
    return result;
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
NsTclReturnMovedObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn     *conn = NULL;
    char        *location;
    int          result;
    Ns_ObjvSpec  args[] = {
        {"location",  Ns_ObjvString, &location, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;
    } else {
        result = Result(interp, Ns_ConnReturnMoved(conn, location));
    }

    return  result;
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
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
NsTclReturnNoticeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn      *conn = NULL;
    int           httpStatus, result;
    char         *title, *message;
    Ns_ObjvSpec   args[] = {
        {"status",   Ns_ObjvInt,    &httpStatus,   NULL},
        {"title",    Ns_ObjvString, &title,   NULL},
        {"message",  Ns_ObjvString, &message, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;
    } else {
        result = Result(interp, Ns_ConnReturnNotice(conn, httpStatus, title, message));
    }
    return result;
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
NsTclReturnRedirectObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn       *conn = NULL;
    int            result;
    char          *location;
    Ns_ObjvSpec    args[] = {
        {"location",  Ns_ObjvString, &location, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;
    } else {
        result = Result(interp, Ns_ConnReturnRedirect(conn, location));
    }
    
    return result;
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
NsTclInternalRedirectObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn       *conn = NULL;
    int            result;
    char          *location;
    Ns_ObjvSpec    args[] = {
        {"location",  Ns_ObjvString, &location, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK
        ) {
        result = TCL_ERROR;

    } else {
        result = Result(interp, Ns_ConnRedirect(conn, location));
    }    

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Result --
 *
 *      Set interp result based on NaviSever result. When the result is NS_OK
 *      set result to as 1, otherwise to 0.
 *
 * Results:
 *      Standard Tcl result code
 *
 * Side effects:
 *      .
 *
 *----------------------------------------------------------------------
 */
static int
Result(Tcl_Interp *interp, Ns_ReturnCode result)
{
    NS_NONNULL_ASSERT(interp != NULL);
    
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj((result == NS_OK) ? 1 : 0));
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
