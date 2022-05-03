/*
 * The contents of this file are subject to the NaviServer Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is NaviServer Code and related documentation
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

static int RegisterPage(const ClientData clientData, const char *method,
                        const char *url, const char *file, const Ns_Time *expiresPtr,
                        unsigned int rflags, unsigned int aflags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static Ns_ReturnCode PageRequest(Ns_Conn *conn, const char *fileName, const Ns_Time *expiresPtr,
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

Ns_ReturnCode
Ns_AdpRequest(Ns_Conn *conn, const char *fileName)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(fileName != NULL);

    return PageRequest(conn, fileName, NULL, 0u);
}

Ns_ReturnCode
Ns_AdpRequestEx(Ns_Conn *conn, const char *fileName, const Ns_Time *expiresPtr)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(fileName != NULL);

    return PageRequest(conn, fileName, expiresPtr, 0u);
}

static Ns_ReturnCode
PageRequest(Ns_Conn *conn, const char *fileName, const Ns_Time *expiresPtr, unsigned int aflags)
{
    const Conn     *connPtr;
    NsServer       *servPtr;
    bool            fileNotFound;
    Tcl_DString     ds, *dsPtr = NULL;
    Ns_ReturnCode   status;

    NS_NONNULL_ASSERT(conn != NULL);

    connPtr = (const Conn *) conn;
    servPtr = connPtr->poolPtr->servPtr;

    /*
     * Verify the file exists.
     */

    if (fileName == NULL) {
        fileNotFound = NS_TRUE;

    } else if (access(fileName, R_OK) == 0) {
        fileNotFound = NS_FALSE;

    } else if (servPtr->adp.defaultExtension != NULL) {

        Tcl_DStringInit(&ds);
        dsPtr = &ds;

        Tcl_DStringAppend(dsPtr, fileName, -1);
        Tcl_DStringAppend(dsPtr, servPtr->adp.defaultExtension, -1);

        if (access(dsPtr->string, R_OK) == 0) {
            fileName = dsPtr->string;
            fileNotFound = NS_FALSE;
        } else {
            fileNotFound = NS_TRUE;
        }
    } else {
        fileNotFound = NS_TRUE;
    }

    if (fileNotFound) {
        if (((Conn *)conn)->recursionCount == 1) {
            Ns_Log(Warning, "AdpPageRequest for '%s' returns 404", fileName);
        }
        Ns_Log(Debug, "AdpPageRequest for '%s' returns 404", fileName);
        status = Ns_ConnReturnNotFound(conn);

    } else {
        Tcl_Interp     *interp = Ns_GetConnInterp(conn);
        NsInterp       *itPtr = NsGetInterpData(interp);
        const char     *type, *start;
        Tcl_Obj        *objv[2];
        int             result;

        Ns_Log(Debug, "AdpPageRequest for '%s' access ok", fileName);

        /*
         * Set the output type based on the file type.
         */

        type = Ns_GetMimeType(fileName);
        if (type == NULL || STREQ(type, "*/*")) {
            type = NSD_TEXTHTML;
        }
        Ns_ConnSetEncodedTypeHeader(conn, type);

        /*
         * Enable TclPro debugging if requested.
         */

        servPtr = connPtr->poolPtr->servPtr;
        if ((servPtr->adp.flags & ADP_DEBUG) != 0u &&
            conn->request.method != NULL &&
            STREQ(conn->request.method, "GET")) {
            const Ns_Set *query = Ns_ConnGetQuery(interp, conn, NULL); /* currently ignoring encoding errors */

            if (query != NULL) {
                itPtr->adp.debugFile = Ns_SetIGet(query, "debug");
            }
        }

        /*
         * Include the ADP with the special start page and null args.
         */

        itPtr->adp.flags |= aflags;
        itPtr->adp.conn = conn;
        start = ((servPtr->adp.startpage != NULL) ? servPtr->adp.startpage : fileName);
        objv[0] = Tcl_NewStringObj(start, -1);
        objv[1] = Tcl_NewStringObj(fileName, -1);
        Tcl_IncrRefCount(objv[0]);
        Tcl_IncrRefCount(objv[1]);
        result = NsAdpInclude(itPtr, 2, objv, start, expiresPtr);
        Tcl_DecrRefCount(objv[0]);
        Tcl_DecrRefCount(objv[1]);

        if (itPtr->adp.exception == ADP_TIMEOUT) {
            Ns_Log(Ns_LogTimeoutDebug, "ADP request %s lead to a timeout", conn->request.line);
            status = Ns_ConnReturnUnavailable(conn);
            Tcl_ResetResult(interp);

        } else if (NsAdpFlush(itPtr, NS_FALSE) != TCL_OK || result != TCL_OK) {
            status = NS_ERROR;
        } else {
            status = NS_OK;
        }
    }

    if (dsPtr != NULL) {
        Tcl_DStringFree(dsPtr);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterAdpObjCmd, NsTclRegisterTclObjCmd --
 *
 *      Implements "ns_register_adp" and "ns_register_tcl".
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
NsTclRegisterAdpObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char          *method, *url, *file = NULL;
    int            noinherit = 0, result;
    unsigned int   aflags = 0u;
    Ns_Time       *expiresPtr = NULL;
    Ns_ObjvSpec    opts[] = {
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
        result = TCL_ERROR;

    } else {
        unsigned int rflags = 0u;

        if (noinherit != 0) {
            rflags |= NS_OP_NOINHERIT;
        }
        result = RegisterPage(clientData, method, url, file, expiresPtr, rflags, aflags);
    }
    return result;
}

int
NsTclRegisterTclObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         noinherit = 0, result;
    char       *method, *url, *file = NULL;
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
        result = TCL_ERROR;
    } else {
        unsigned int rflags = 0u;

        if (noinherit != 0) {
            rflags |= NS_OP_NOINHERIT;
        }
        result = RegisterPage(clientData, method, url, file, NULL, rflags, ADP_TCLFILE);
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * RegisterPage --
 *
 *      Register ADP page
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
RegisterPage(const ClientData clientData,
             const char *method, const char *url, const char *file,
             const Ns_Time *expiresPtr, unsigned int rflags, unsigned int aflags)
{
    const NsInterp *itPtr = clientData;
    AdpRequest     *adp;
    size_t          fileLength;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    fileLength = (file == NULL) ? 0u : strlen(file);
    adp = ns_calloc(1u, sizeof(AdpRequest) + fileLength + 1u);
    if (file != NULL) {
        memcpy(adp->file, file, fileLength + 1u);
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

Ns_ReturnCode
NsAdpPageProc(const void *arg, Ns_Conn *conn)
{
    const AdpRequest *adp = arg;
    const Ns_Time    *expiresPtr;
    Ns_DString        ds;
    const char       *fileName, *server;
    Ns_ReturnCode     status;

    NS_NONNULL_ASSERT(conn != NULL);

    server = Ns_ConnServer(conn);
    Ns_DStringInit(&ds);

    if (adp->file[0] == '\0') {
        if (Ns_UrlToFile(&ds, server, conn->request.url) != NS_OK) {
            fileName = NULL;
        } else {
            fileName = ds.string;
        }
    } else if (Ns_PathIsAbsolute(adp->file) == NS_FALSE) {
        fileName = Ns_PagePath(&ds, server, adp->file, (char *)0L);
    } else {
        fileName = adp->file;
    }

    if (adp->expires.sec > 0 || adp->expires.usec > 0) {
        expiresPtr = &adp->expires;
    } else {
        expiresPtr = NULL;
    }

    status = PageRequest(conn, fileName, expiresPtr, adp->flags);

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
NsAdpPageArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const AdpRequest *adp = arg;
    size_t            i;

    Ns_DStringPrintf(dsPtr, " %" PRId64 ":%ld",
                     (int64_t) adp->expires.sec,
                     adp->expires.usec);

    Tcl_DStringAppendElement(dsPtr, adp->file);

    Tcl_DStringStartSublist(dsPtr);
    if ((adp->flags & ADP_TCLFILE) != 0u) {
        Tcl_DStringAppendElement(dsPtr, "tcl");
    }
    for (i = 0u; i < (sizeof(adpOpts) / sizeof(adpOpts[0])); i++) {
        if ((adp->flags & adpOpts[i].value) != 0u) {
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
Ns_AdpFlush(Tcl_Interp *interp, bool doStream)
{
    NsInterp *itPtr;
    int       result;

    itPtr = NsGetInterpData(interp);
    if (likely(itPtr != NULL)) {
        result = NsAdpFlush(itPtr, doStream);
    } else {
        Ns_TclPrintfResult(interp, "not a server interp");
        result = TCL_ERROR;
    }
    return result;
}

int
NsAdpFlush(NsInterp *itPtr, bool doStream)
{
    const Ns_Conn *conn;
    Tcl_Interp    *interp;
    int            len, result = TCL_ERROR;
    unsigned int   flags;
    char          *buf;

    NS_NONNULL_ASSERT(itPtr != NULL);

    interp = itPtr->interp;
    flags = itPtr->adp.flags;

    /*
     * Verify output context.
     */

    conn = (itPtr->adp.conn != NULL) ? itPtr->adp.conn : itPtr->conn;

    if (conn == NULL) {
        assert(itPtr->adp.chan == NULL);
        Ns_TclPrintfResult(interp, "no ADP output context");
        return TCL_ERROR;
    }
    assert(conn != NULL);

    buf = itPtr->adp.output.string;
    len = itPtr->adp.output.length;

    /*
     * Nothing to do for zero length buffer except reset
     * if this is the last flush.
     */

    if (len < 1 && (flags & ADP_FLUSHED) != 0u) {
        if (!doStream) {
            NsAdpReset(itPtr);
        }
        return TCL_OK;
    }

    /*
     * If enabled, trim leading whitespace if no content has been sent yet.
     */

    if ((flags & ADP_TRIM) != 0u && (flags & ADP_FLUSHED) == 0u) {
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
     * reset ADP output and do not send anything
     */

    Tcl_ResetResult(interp);

    if (itPtr->adp.exception == ADP_ABORT) {
        Ns_TclPrintfResult(interp, "ADP flush disabled: ADP aborted");

    } else if ((conn->flags & NS_CONN_SENT_VIA_WRITER) != 0u || (len == 0 && doStream)) {
        result = TCL_OK;

    } else {
        if (itPtr->adp.chan != NULL) {
            while (len > 0) {
                int wrote = Tcl_Write(itPtr->adp.chan, buf, len);
                if (wrote < 0) {
                    Ns_TclPrintfResult(interp, "write failed: %s", Tcl_PosixError(interp));
                    break;
                }
                buf += wrote;
                len -= wrote;
            }
            if (len == 0) {
                result = TCL_OK;
            }
        } else {
            if ((conn->flags & NS_CONN_CLOSED) != 0u) {
                result = TCL_OK;
                Ns_TclPrintfResult(interp, "adp flush failed: connection closed");
            } else {
                struct iovec sbuf;

                if ((flags & ADP_FLUSHED) == 0u && (flags & ADP_EXPIRE) != 0u) {
                    Ns_ConnCondSetHeaders(conn, "Expires", "now");
                }

                if ((conn->flags & NS_CONN_SKIPBODY) != 0u) {
                    buf = NULL;
                    len = 0;
                }

                sbuf.iov_base = buf;
                sbuf.iov_len  = (size_t)len;
                if (Ns_ConnWriteVChars(itPtr->conn, &sbuf, 1,
                                       (doStream ? NS_CONN_STREAM : 0u)) == NS_OK) {
                    result = TCL_OK;
                }
                if (result != TCL_OK) {
                    Ns_TclPrintfResult(interp, "adp flush failed: connection flush error");
                }
            }
        }
        itPtr->adp.flags |= ADP_FLUSHED;

        /*
         * Raise an abort exception if autoabort is enabled.
         */

        if (result != TCL_OK && (flags & ADP_AUTOABORT) != 0u) {
            Tcl_AddErrorInfo(interp, "\n    abort exception raised");
            NsAdpLogError(itPtr);
            itPtr->adp.exception = ADP_ABORT;
        }
    }
    Tcl_DStringSetLength(&itPtr->adp.output, 0);

    if (!doStream) {
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
