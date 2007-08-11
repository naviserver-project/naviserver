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

NS_RCSID("@(#) $Header$");


/*
 * The following structure allows a single ADP or Tcl page to
 * be requested via multiple URLs.
 */

typedef struct AdpRequest {
    Ns_Time  expires;     /* Time to live for cached output. */
    int      flags;       /* ADP options. */
    char     file[1];     /* Optional, path to specific page. */
} AdpRequest;


/*
 * Static functions defined in this file.
 */

static int RegisterPage(ClientData arg, Tcl_Interp *interp,
                        CONST char *method, CONST char *url, CONST char *file,
                        Ns_Time *expiresPtr, int rflags, int aflags);
static int PageRequest(Ns_Conn *conn, CONST char *file, Ns_Time *ttlPtr, int aflags);

/*
 * Static variables defined in this file.
 */

static Ns_ObjvTable adpOpts[] = {
    {"autoabort",    ADP_AUTOABORT},
    {"detailerror",  ADP_DETAIL},
    {"displayerror", ADP_DISPLAY},
    {"expire",       ADP_EXPIRE},
    {"gzip",         ADP_GZIP},
    {"cache",        ADP_CACHE},
    {"safe",         ADP_SAFE},
    {"singlescript", ADP_SINGLE},
    {"stricterror",  ADP_STRICT},
    {"trace",        ADP_TRACE},
    {"trimspace",    ADP_TRIM},
    {"stream",       ADP_STREAM},
    {NULL, 0}
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
Ns_AdpRequest(Ns_Conn *conn, CONST char *file)
{
    return PageRequest(conn, file, NULL, 0);
}

int
Ns_AdpRequestEx(Ns_Conn *conn, CONST char *file, Ns_Time *expiresPtr)
{
    return PageRequest(conn, file, expiresPtr, 0);
}

static int
PageRequest(Ns_Conn *conn, CONST char *file, Ns_Time *expiresPtr, int aflags)
{
    Conn         *connPtr = (Conn *) conn;
    Tcl_Interp   *interp;
    NsInterp     *itPtr;
    char         *start, *type;
    Ns_Set       *query;
    NsServer     *servPtr;
    Tcl_Obj      *objv[2];
    int           result;

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
    Ns_ConnSetEncodedTypeHeader(conn, type);

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

    itPtr->adp.flags |= aflags;
    itPtr->adp.conn = conn;
    start = (char*)(servPtr->adp.startpage ? servPtr->adp.startpage : file);
    objv[0] = Tcl_NewStringObj(start, -1);
    objv[1] = Tcl_NewStringObj(file, -1);
    Tcl_IncrRefCount(objv[0]);
    Tcl_IncrRefCount(objv[1]);
    result = NsAdpInclude(itPtr, 2, objv, start, expiresPtr);
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
NsTclRegisterAdpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char       *method, *url, *file = NULL;
    int         rflags = 0, aflags = 0;
    Ns_Time    *expiresPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &rflags,     (void *) NS_OP_NOINHERIT},
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
    return RegisterPage(arg, interp, method, url, file,
                        expiresPtr, rflags, aflags);
}

int
NsTclRegisterTclObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    int   rflags = 0;
    char *method, *url, *file = NULL;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &rflags, (void *) NS_OP_NOINHERIT},
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
    return RegisterPage(arg, interp, method, url, file,
                        NULL, rflags, ADP_TCLFILE);
}

static int
RegisterPage(ClientData arg, Tcl_Interp *interp,
             CONST char *method, CONST char *url, CONST char *file,
             Ns_Time *expiresPtr, int rflags, int aflags)
{
    NsInterp   *itPtr = arg;
    AdpRequest *adp;

    adp = ns_calloc(1, sizeof(AdpRequest) + (file ? strlen(file) : 0));
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
    char       *file;
    int         status;

    Ns_DStringInit(&ds);

    if (adp->file[0] == '\0') {
        if (Ns_UrlToFile(&ds, Ns_ConnServer(conn), conn->request->url)
            != NS_OK) {
            file = NULL;
        } else {
            file = ds.string;
        }
    } else if (!Ns_PathIsAbsolute(adp->file)) {
        file = Ns_PagePath(&ds, Ns_ConnServer(conn), adp->file, NULL);
    }
    if (file == NULL) {
        status = Ns_ConnReturnInternalError(conn);
        goto done;
    }

    if (adp->expires.sec > 0 || adp->expires.usec > 0) {
        expiresPtr = &adp->expires;
    } else {
        expiresPtr = NULL;
    }

    status = PageRequest(conn, file, expiresPtr, adp->flags);

 done:
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
    AdpRequest *adp = arg;
    int         i;

    Ns_DStringPrintf(dsPtr, "%jd:%ld",
                     (intmax_t) adp->expires.sec,
                     adp->expires.usec);

    Tcl_DStringAppendElement(dsPtr, adp->file);

    Tcl_DStringStartSublist(dsPtr);
    if (adp->flags & ADP_TCLFILE) {
        Tcl_DStringAppendElement(dsPtr, "tcl");
    }
    for (i = 0; i < (sizeof(adpOpts) / sizeof(adpOpts[0])); i++) {
        if (adp->flags & adpOpts[i].value) {
            Tcl_DStringAppendElement(dsPtr, adpOpts[i].key);
        }
    }
    Tcl_DStringEndSublist(dsPtr);

}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpFlush --
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
NsAdpFlush(NsInterp *itPtr, int stream)
{
    Ns_Conn    *conn;
    Ns_DString  cds;
    Tcl_Interp *interp = itPtr->interp;
    int         len, wrote, gzip = 0, result = TCL_ERROR, flags = itPtr->adp.flags;
    char       *buf, *ahdr;

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

                        gzip = 1;
                    }
                }

                if (!(flags & ADP_FLUSHED) && (flags & ADP_EXPIRE)) {
                    Ns_ConnCondSetHeaders(conn, "Expires", "now");
                }

                if (conn->flags & NS_CONN_SKIPBODY) {
                    buf = NULL;
                    len = 0;
                }

                if (gzip) {
                    if (Ns_ConnWriteData(itPtr->conn, buf, len,
                                         stream ? NS_CONN_STREAM : 0) == NS_OK) {
                        result = TCL_OK;
                    }
                } else {
                    if (Ns_ConnWriteChars(itPtr->conn, buf, len,
                                          stream ? NS_CONN_STREAM : 0) == NS_OK) {
                        result = TCL_OK;
                    }
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
    }
    return result;
}
