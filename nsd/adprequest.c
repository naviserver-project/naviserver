/*
 * The contents of this file are subject to the Naviserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is Naviserver Code and related documentation
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
 * adprequest.c --
 *
 *	ADP connection request support.
 */


#include "nsd.h"

NS_RCSID("@(#) $Header$");

/*
 * Static functions defined in this file.
 */


/*
 *----------------------------------------------------------------------
 *
 * NsAdpProc --
 *
 *	Check for a normal file and call Ns_AdpRequest.
 *
 * Results:
 *	A standard Naviserver request result.
 *
 * Side effects:
 *	Depends on code embedded within page.
 *
 *----------------------------------------------------------------------
 */

int
NsAdpProc(void *arg, Ns_Conn *conn)
{
    Ns_Time *ttlPtr = arg;
    Ns_DString file;
    int status;

    Ns_DStringInit(&file);
    Ns_UrlToFile(&file, Ns_ConnServer(conn), conn->request->url);
    status = Ns_AdpRequestEx(conn, file.string, ttlPtr, 0);
    Ns_DStringFree(&file);
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclProc --
 *
 *	Check for a normal file and call Ns_AdpRequest.
 *
 * Results:
 *	A standard Naviserver request result.
 *
 * Side effects:
 *	Depends on code embedded within page.
 *
 *----------------------------------------------------------------------
 */

int
NsAdpTclProc(void *arg, Ns_Conn *conn)
{
    Ns_Time *ttlPtr = arg;
    Ns_DString file;
    int status;

    Ns_DStringInit(&file);
    Ns_UrlToFile(&file, Ns_ConnServer(conn), conn->request->url);
    status = Ns_AdpRequestEx(conn, file.string, ttlPtr, ADP_TCLFILE);
    Ns_DStringFree(&file);
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AdpRequest, Ns_AdpRequestEx -
 *
 *  	Invoke a file for an ADP request with an optional cache
 *	timeout.
 *
 * Results:
 *	A standard Naviserver request result.
 *
 * Side effects:
 *	Depends on code embedded within page.
 *
 *----------------------------------------------------------------------
 */

int
Ns_AdpRequest(Ns_Conn *conn, CONST char *file)
{
    return Ns_AdpRequestEx(conn, file, NULL, 0);
}

int
Ns_AdpRequestEx(Ns_Conn *conn, CONST char *file, Ns_Time *ttlPtr, int flags)
{
    Conn	     *connPtr = (Conn *) conn;
    Tcl_Interp       *interp;
    NsInterp         *itPtr;
    char             *start, *type;
    Ns_Set           *query;
    NsServer	     *servPtr;
    Tcl_Obj	     *objv[2];
    int		      result;

    interp = Ns_GetConnInterp(conn);
    itPtr = NsGetInterpData(interp);

    /*
     * Verify the file exists.
     */

    if (access(file, R_OK) != 0) {
	return Ns_ConnReturnNotFound(conn);
    }

    /*
     * Set the output type based on the file type.
     */

    type = Ns_GetMimeType(file);
    if (type == NULL || STREQ(type, "*/*")) {
	type = NSD_TEXTHTML;
    }
    Ns_ConnSetTypeHeader(conn, type);

    /*
     * Enable TclPro debugging if requested.
     */

    servPtr = connPtr->servPtr;
    if ((itPtr->servPtr->adp.flags & ADP_DEBUG) &&
	STREQ(conn->request->method, "GET") &&
	(query = Ns_ConnGetQuery(conn)) != NULL) {
	itPtr->adp.debugFile = Ns_SetIGet(query, "debug");
    }

    /*
     * Include the ADP with the special start page and null args.
     */

    itPtr->adp.flags |= flags;
    itPtr->adp.conn = conn;
    start = (char*)(servPtr->adp.startpage ? servPtr->adp.startpage : file);
    objv[0] = Tcl_NewStringObj(start, -1);
    objv[1] = Tcl_NewStringObj(file, -1);
    Tcl_IncrRefCount(objv[0]);
    Tcl_IncrRefCount(objv[1]);
    result = NsAdpInclude(itPtr, 2, objv, start, ttlPtr);
    Tcl_DecrRefCount(objv[0]);
    Tcl_DecrRefCount(objv[1]);
    if (NsAdpFlush(itPtr, 0) != TCL_OK || result != TCL_OK) {
	return NS_ERROR;
    }
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpFlush --
 *
 *	Flush output to connection response buffer.
 *
 * Results:
 *	TCL_ERROR if flush failed, TCL_OK otherwise.
 *
 * Side effects:
 *  	Output buffer is truncated in all cases.
 *
 *----------------------------------------------------------------------
 */

int
NsAdpFlush(NsInterp *itPtr, int stream)
{
    Ns_Conn *conn;
    Ns_DString cds;
    Tcl_Interp *interp = itPtr->interp;
    int len, wrote, result = TCL_ERROR, flags = itPtr->adp.flags;
    char *buf, *ahdr;

    /*
     * Verify output context.
     */

    conn = itPtr->adp.conn ? itPtr->adp.conn : itPtr->conn;

    if (conn == NULL && itPtr->adp.chan == NULL) {
	Tcl_SetResult(interp, "no adp output context", TCL_STATIC);
	return TCL_ERROR;
    }
    buf = itPtr->adp.output.string;
    len = itPtr->adp.output.length;

    /*
     * If enabled, trim leading whitespace if no content has been sent yet.
     */

    if ((flags & ADP_TRIM) && !(flags & ADP_FLUSHED)) {
	while (len > 0 && isspace(UCHAR(*buf))) {
	    ++buf;
	    --len;
	}
    }

    /*
     * Leave error messages if output is disabled or failed. Otherwise,
     * send data if there's any to send or stream is 0, indicating this
     * is the final flush call.
     */

    Ns_DStringInit(&cds);
    Tcl_ResetResult(interp);

    if (itPtr->adp.exception == ADP_ABORT) {
	Tcl_SetResult(interp, "adp flush disabled: adp aborted", TCL_STATIC);
    } else if (len == 0 && stream) {
	result = TCL_OK;
    } else {
	if (itPtr->adp.chan != NULL) {
	    while (len > 0) {
		wrote = Tcl_Write(itPtr->adp.chan, buf, len);
		if (wrote < 0) {
	    	    Tcl_AppendResult(interp, "write failed: ",
				     Tcl_PosixError(interp), NULL);
		    break;
		}
		buf += wrote;
		len -= wrote;
	    }
	    if (len == 0) {
		result = TCL_OK;
	    }
	} else {
	    if (conn->flags & NS_CONN_CLOSED) {
                result = TCL_OK;
                Tcl_SetResult(interp, "adp flush failed: connection closed",
			      TCL_STATIC);
	    } else {

                if (flags & ADP_GZIP) {

                    /*
                     * Should we compress the response?  If the ADP requested it with
                     * ns_adp_compress, it's enabled in the server config, if headers
                     * haven't been sent yet, if this isn't a HEAD request, if streaming
                     * isn't turned on, if the response meets the minimum size per the
                     * config, if the browser indicates it can accept it, only THEN do
                     * we compress the response.
                     */

                    if (!(conn->flags & NS_CONN_SENTHDRS)
                        && !(conn->flags & NS_CONN_SKIPBODY)
                        && !stream
                        && len >= itPtr->servPtr->adp.compress.minsize
                        && (ahdr = Ns_SetIGet(Ns_ConnHeaders(conn),
                                              "Accept-Encoding")) != NULL
                        && strstr(ahdr, "gzip") != NULL
                        && Ns_CompressGzip(buf, len, &cds,
                                           itPtr->servPtr->adp.compress.level) == NS_OK) {

                        /*
                         * We may want to check if Content-Encoding was already
                         * set, and if so, don't gzip.
                         */

                        Ns_ConnCondSetHeaders(conn, "Content-Encoding", "gzip");
                        buf = cds.string;
                        len = cds.length;
                    }

	    	}

                /*
                 * Flush out the headers now that the encoded output length
                 * is known for non-streaming output.
                 */

                if (!(conn->flags & NS_CONN_SENTHDRS)) {

                    /* Switch to chunked mode if browser supports chunked encoding and
                     * streaming is enabled.
                     */

                    if (stream && itPtr->conn->request->version > 1.0) {
                        Ns_ConnSetChunkedFlag(itPtr->conn, 1);
                    }
                    Ns_ConnSetRequiredHeaders(conn, NULL, stream ? -1 : len);
                    Ns_ConnQueueHeaders(conn, 200);
	    	}

                if (!(flags & ADP_FLUSHED) && (flags & ADP_EXPIRE)) {
		    Ns_ConnCondSetHeaders(conn, "Expires", "now");
	    	}

                if (conn->flags & NS_CONN_SKIPBODY) {
                    buf = NULL;
                    len = 0;
                }

	    	if (Ns_WriteConn(itPtr->conn, buf, len) == NS_OK) {
		    result = TCL_OK;
	    	} else {
	    	    Tcl_SetResult(interp,
				  "adp flush failed: connection flush error",
				  TCL_STATIC);
	    	}
	    }
	}
	itPtr->adp.flags |= ADP_FLUSHED;

	/*
	 * Raise an abort exception if autoabort is enabled.
	 */

    	if (result != TCL_OK && (flags & ADP_AUTOABORT)) {
	    Tcl_AddErrorInfo(interp, "\n    abort exception raised");
	    NsAdpLogError(itPtr);
	    itPtr->adp.exception = ADP_ABORT;
    	}
    }
    Tcl_DStringTrunc(&itPtr->adp.output, 0);
    Ns_DStringFree(&cds);

    if (!stream) {
        NsAdpReset(itPtr);
        result = Ns_ConnClose(conn);
    }
    return result;
}

