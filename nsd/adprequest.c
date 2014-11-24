/*
 * The contents of this file are subject to the Naviserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
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
 *      ADP connection request support.
 */


#include "nsd.h"

/*
 * The following structure allows a single ADP or Tcl page to
 * be requested via multiple URLs.
 */

typedef struct AdpRequest {
    Ns_Time       expires;     /* Time to live for cached output. */
    unsigned int  flags;       /* ADP options. */
    char          file[1];     /* Optional, path to specific page. */
} AdpRequest;


/*
 * Static functions defined in this file.
 */

static int RegisterPage(ClientData arg, const char *method, 
			const char *url, const char *file, const Ns_Time *expiresPtr, 
			unsigned int rflags, unsigned int aflags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int PageRequest(Ns_Conn *conn, const char *file, const Ns_Time *expiresPtr, 
		       unsigned int aflags)
    NS_GNUC_NONNULL(1);

/*
 * Static variables defined in this file.
 */

static Ns_ObjvTable adpOpts[] = {
    {"autoabort",    ADP_AUTOABORT},
    {"detailerror",  ADP_DETAIL},
    {"displayerror", ADP_DISPLAY},
    {"expire",       ADP_EXPIRE},
    {"cache",        ADP_CACHE},
    {"safe",         ADP_SAFE},
    {"singlescript", ADP_SINGLE},
    {"stricterror",  ADP_STRICT},
    {"trace",        ADP_TRACE},
    {"trimspace",    ADP_TRIM},
    {"stream",       ADP_STREAM},
    {NULL, 0u}
};



/*
 *----------------------------------------------------------------------
 *
 * Ns_AdpRequest, Ns_AdpRequestEx -
 *
 *      Invoke a file for an ADP request with an optional cache
 *      timeout.
 *
 * Results:
 *      A standard request result.
 *
 * Side effects:
 *      Depends on code embedded within page.
 *
 *----------------------------------------------------------------------
 */

int
Ns_AdpRequest(Ns_Conn *conn, const char *file)
{
    assert(conn != NULL);
    assert(file != NULL);

    return PageRequest(conn, file, NULL, 0U);
}

int
Ns_AdpRequestEx(Ns_Conn *conn, const char *file, const Ns_Time *expiresPtr)
{
    assert(conn != NULL);
    assert(file != NULL);

    return PageRequest(conn, file, expiresPtr, 0U);
}

static int
PageRequest(Ns_Conn *conn, const char *file, const Ns_Time *expiresPtr, unsigned int aflags)
{
    Conn         *connPtr = (Conn *) conn;
    Tcl_Interp   *interp;
    NsInterp     *itPtr;
    char         *type;
    const char   *start;
    Ns_Set       *query;
    NsServer     *servPtr;
    Tcl_Obj      *objv[2];
    int           result;

    assert(connPtr != NULL);

    /*
     * Verify the file exists.
     */

    if (file == NULL || access(file, R_OK) != 0) {
        return Ns_ConnReturnNotFound(conn);
    }

    interp = Ns_GetConnInterp(conn);
    itPtr = NsGetInterpData(interp);


    /*
     * Set the output type based on the file type.
     */

    type = Ns_GetMimeType(file);
    if (type == NULL || STREQ(type, "*/*")) {
        type = NSD_TEXTHTML;
    }
    Ns_ConnSetEncodedTypeHeader(conn, type);

    /*
     * Enable TclPro debugging if requested.
     */

    servPtr = connPtr->poolPtr->servPtr;
    if ((servPtr->adp.flags & ADP_DEBUG) != 0U &&
        conn->request->method != NULL &&
        STREQ(conn->request->method, "GET") &&
        (query = Ns_ConnGetQuery(conn)) != NULL) {
        itPtr->adp.debugFile = Ns_SetIGet(query, "debug");
    }

    /*
     * Include the ADP with the special start page and null args.
     */

    itPtr->adp.flags |= aflags;
    itPtr->adp.conn = conn;
    start = ((servPtr->adp.startpage != NULL) ? servPtr->adp.startpage : file);
    objv[0] = Tcl_NewStringObj(start, -1);
    objv[1] = Tcl_NewStringObj(file, -1);
    Tcl_IncrRefCount(objv[0]);
    Tcl_IncrRefCount(objv[1]);
    result = NsAdpInclude(itPtr, 2, objv, start, expiresPtr);
    Tcl_DecrRefCount(objv[0]);
    Tcl_DecrRefCount(objv[1]);

    if (itPtr->adp.exception == ADP_TIMEOUT) {
        return Ns_ConnReturnUnavailable(conn);
    }

    if (NsAdpFlush(itPtr, 0) != TCL_OK || result != TCL_OK) {
        return NS_ERROR;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterAdpObjCmd, NsTclRegisterTclObjCmd --
 *
 *      Implements ns_register_adp and ns_register_tcl.
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
NsTclRegisterAdpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    char         *method, *url, *file = NULL;
    int           noinherit = 0;
    unsigned int  rflags = 0U, aflags = 0U;
    Ns_Time      *expiresPtr = NULL;

    Ns_ObjvSpec opts[] = {
	{"-noinherit", Ns_ObjvBool,  &noinherit,  INT2PTR(NS_TRUE)},
        {"-expires",   Ns_ObjvTime,  &expiresPtr, NULL},
        {"-options",   Ns_ObjvFlags, &aflags,     adpOpts},
        {"--",         Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",   Ns_ObjvString, &method,   NULL},
        {"url",      Ns_ObjvString, &url,      NULL},
        {"?file",    Ns_ObjvString, &file,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit != 0) {rflags |= NS_OP_NOINHERIT;}
    return RegisterPage(arg, method, url, file, expiresPtr, rflags, aflags);
}

int
NsTclRegisterTclObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          noinherit = 0;
    unsigned int rflags = 0U;
    char        *method, *url, *file = NULL;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &noinherit, INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",   Ns_ObjvString, &method,   NULL},
        {"url",      Ns_ObjvString, &url,      NULL},
        {"?file",    Ns_ObjvString, &file,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit != 0) {rflags |= NS_OP_NOINHERIT;}
    return RegisterPage(arg, method, url, file, NULL, rflags, ADP_TCLFILE);
}

static int
RegisterPage(ClientData arg,
             const char *method, const char *url, const char *file,
             const Ns_Time *expiresPtr, unsigned int rflags, unsigned int aflags)
{
    NsInterp   *itPtr = arg;
    AdpRequest *adp;

    assert(itPtr != NULL);
    assert(method != NULL);
    assert(url != NULL);

    adp = ns_calloc(1U, sizeof(AdpRequest) + ((file != NULL) ? strlen(file) : 0U));
    if (file != NULL) {
        strcpy(adp->file, file);
    }
    if (expiresPtr != NULL) {
        adp->expires = *expiresPtr;
    }
    adp->flags = aflags;

    Ns_RegisterRequest(itPtr->servPtr->server, method, url,
                       NsAdpPageProc, ns_free, adp, rflags);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpPageProc --
 *
 *      Check for a normal ADP or Tcl file and call AdpRequest
 *      accordingly.
 *
 * Results:
 *      A standard request result.
 *
 * Side effects:
 *      Depends on code embedded within page.
 *
 *----------------------------------------------------------------------
 */

int
NsAdpPageProc(void *arg, Ns_Conn *conn)
{
    AdpRequest *adp = arg;
    Ns_Time    *expiresPtr;
    Ns_DString  ds;
    const char *file = NULL, *server = Ns_ConnServer(conn);
    int         status;

    Ns_DStringInit(&ds);

    if (adp->file[0] == '\0') {
        if (Ns_UrlToFile(&ds, server, conn->request->url) != NS_OK) {
            file = NULL;
        } else {
            file = ds.string;
        }
    } else if (Ns_PathIsAbsolute(adp->file) == NS_FALSE) {
        file = Ns_PagePath(&ds, server, adp->file, NULL);
    } else {
        file = adp->file;
    }

    if (adp->expires.sec > 0 || adp->expires.usec > 0) {
        expiresPtr = &adp->expires;
    } else {
        expiresPtr = NULL;
    }

    status = PageRequest(conn, file, expiresPtr, adp->flags);

    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpPageArgProc --
 *
 *      Proc info callback for ADP pages.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsAdpPageArgProc(Tcl_DString *dsPtr, void *arg)
{
    AdpRequest   *adp = arg;
    unsigned int  i;

    Ns_DStringPrintf(dsPtr, " %" PRIu64 ":%ld",
                     (int64_t) adp->expires.sec,
                     adp->expires.usec);

    Tcl_DStringAppendElement(dsPtr, adp->file);

    Tcl_DStringStartSublist(dsPtr);
    if ((adp->flags & ADP_TCLFILE) != 0U) {
        Tcl_DStringAppendElement(dsPtr, "tcl");
    }
    for (i = 0; i < (sizeof(adpOpts) / sizeof(adpOpts[0])); i++) {
	if ((adp->flags & adpOpts[i].value) != 0U) {
            Tcl_DStringAppendElement(dsPtr, adpOpts[i].key);
        }
    }
    Tcl_DStringEndSublist(dsPtr);

}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AdpFlush, NsAdpFlush --
 *
 *      Flush output to connection response buffer.
 *
 * Results:
 *      TCL_ERROR if flush failed, TCL_OK otherwise.
 *
 * Side effects:
 *      Output buffer is truncated in all cases.
 *
 *----------------------------------------------------------------------
 */

int
Ns_AdpFlush(Tcl_Interp *interp, int isStreaming)
{
    NsInterp *itPtr;

    itPtr = NsGetInterpData(interp);
    if (itPtr == NULL) {
        Tcl_SetResult(interp, "not a server interp", TCL_STATIC);
        return TCL_ERROR;
    }
    return NsAdpFlush(itPtr, isStreaming);
}

int
NsAdpFlush(NsInterp *itPtr, int doStream)
{
    Ns_Conn     *conn;
    Tcl_Interp  *interp;
    int          len, result = TCL_ERROR;
    unsigned int flags;
    char        *buf;

    assert(itPtr != NULL);

    interp = itPtr->interp;
    flags = itPtr->adp.flags;

    /*
     * Verify output context.
     */

    conn = (itPtr->adp.conn != NULL) ? itPtr->adp.conn : itPtr->conn;

    if (conn == NULL) {
        assert(itPtr->adp.chan == NULL);
        Tcl_SetResult(interp, "no adp output context", TCL_STATIC);
        return TCL_ERROR;
    }
    assert(conn != NULL);

    buf = itPtr->adp.output.string;
    len = itPtr->adp.output.length;

    /*
     * Nothing to do for zero length buffer except reset
     * if this is the last flush.
     */

    if (len < 1 && (flags & ADP_FLUSHED) != 0U) {
        if (doStream == 0) {
            NsAdpReset(itPtr);
        }
        return NS_OK;
    }

    /*
     * If enabled, trim leading whitespace if no content has been sent yet.
     */

    if ((flags & ADP_TRIM) != 0U && (flags & ADP_FLUSHED) == 0U) {
        while (len > 0 && CHARTYPE(space, *buf) != 0) {
            ++buf;
            --len;
        }
    }

    /*
     * Leave error messages if output is disabled or failed. Otherwise,
     * send data if there's any to send or stream is 0, indicating this
     * is the final flush call.
     *
     * Special case when has been sent via Writer thread, we just need to
     * reset adp output and do not send anything
     */

    Tcl_ResetResult(interp);

    if (itPtr->adp.exception == ADP_ABORT) {
        Tcl_SetResult(interp, "adp flush disabled: adp aborted", TCL_STATIC);
    } else
    if ((conn->flags & NS_CONN_SENT_VIA_WRITER) != 0U || (len == 0 && doStream != 0)) {
        result = TCL_OK;
    } else {
        if (itPtr->adp.chan != NULL) {
            while (len > 0) {
                int wrote = Tcl_Write(itPtr->adp.chan, buf, len);
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
            if ((conn->flags & NS_CONN_CLOSED) != 0U) {
                result = TCL_OK;
                Tcl_SetResult(interp, "adp flush failed: connection closed",
                              TCL_STATIC);
            } else {
		struct iovec sbuf;

                if ((flags & ADP_FLUSHED) == 0U && (flags & ADP_EXPIRE) != 0U) {
                    Ns_ConnCondSetHeaders(conn, "Expires", "now");
                }

                if ((conn->flags & NS_CONN_SKIPBODY) != 0U) {
                    buf = NULL;
                    len = 0;
                }
		
		sbuf.iov_base = buf;
		sbuf.iov_len  = (size_t)len;
                if (Ns_ConnWriteVChars(itPtr->conn, &sbuf, 1, 
                                       (doStream != 0) ? NS_CONN_STREAM : 0) == NS_OK) {
                    result = TCL_OK;
                }
                if (result != TCL_OK) {
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

        if (result != TCL_OK && (flags & ADP_AUTOABORT) != 0U) {
            Tcl_AddErrorInfo(interp, "\n    abort exception raised");
            NsAdpLogError(itPtr);
            itPtr->adp.exception = ADP_ABORT;
        }
    }
    Tcl_DStringTrunc(&itPtr->adp.output, 0);

    if (doStream == 0) {
        NsAdpReset(itPtr);
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
