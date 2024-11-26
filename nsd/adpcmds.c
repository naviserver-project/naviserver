/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
 */

/*
 * adpcmds.c --
 *
 *      ADP commands.
 */

#include "nsd.h"

/*
 * Static variables defined in this file.
 */
static Ns_ObjvValueRange posintRange0 = {0, TCL_SIZE_MAX};

/*
 * Local functions defined in this file.
 */

static int ExceptionObjCmd(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                           AdpResult exception) NS_GNUC_NONNULL(1);
static int GetFrame(const ClientData clientData, AdpFrame **framePtrPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int GetOutput(ClientData clientData, Tcl_DString **dsPtrPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int GetInterp(Tcl_Interp *interp, NsInterp **itPtrPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int AdpFlushObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc,
                          Tcl_Obj *const* objv, bool doStream);

static TCL_OBJCMDPROC_T AdpCtlBufSizeObjCmd;


/*
 *----------------------------------------------------------------------
 *
 * Ns_AdpAppend, NsAdpAppend --
 *
 *      Append content to the ADP output buffer, flushing the content
 *      if necessary.
 *
 * Results:
 *      TCL_ERROR if append and/or flush failed, TCL_OK otherwise.
 *
 * Side effects:
 *      Will set ADP error flag and leave an error message in
 *      the interp on flush failure.
 *
 *----------------------------------------------------------------------
 */

int
Ns_AdpAppend(Tcl_Interp *interp, const char *buf, TCL_SIZE_T len)
{
    NsInterp *itPtr;
    int       status;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    if (unlikely(GetInterp(interp, &itPtr) != TCL_OK)) {
        status = TCL_ERROR;
    } else {
        status = NsAdpAppend(itPtr, buf, len);
    }
    return status;
}

int
NsAdpAppend(NsInterp *itPtr, const char *buf, TCL_SIZE_T len)
{
    Tcl_DString *bufPtr;
    int          status = TCL_OK;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    if (GetOutput(itPtr, &bufPtr) != TCL_OK) {
        status = TCL_ERROR;
    } else {
        Ns_DStringNAppend(bufPtr, buf, len);
        if (
            ((itPtr->adp.flags & ADP_STREAM) != 0u
             || (size_t)bufPtr->length > itPtr->adp.bufsize
             )
            && NsAdpFlush(itPtr, NS_TRUE) != TCL_OK) {
            status = TCL_ERROR;
        }
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AdpGetOutput --
 *
 *      Get the dstring used to buffer ADP content.
 *
 * Results:
 *      TCL_ERROR if no active ADP, TCL_OK otherwise.
 *      doStreamPtr set to 1 if streaming mode active.
 *      maxBufferPtr set to length of buffer before Flush
 *      should be called.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_AdpGetOutput(Tcl_Interp *interp, Tcl_DString **dsPtrPtr,
                int *doStreamPtr, size_t *maxBufferPtr)
{
    NsInterp *itPtr;
    int       status = TCL_OK;

    if (unlikely(GetInterp(interp, &itPtr) != TCL_OK)
        || unlikely(GetOutput(itPtr, dsPtrPtr) != TCL_OK)) {
        status = TCL_ERROR;
    } else {
        if (doStreamPtr != NULL) {
            *doStreamPtr = (itPtr->adp.flags & ADP_STREAM) != 0u ? 1 : 0;
        }
        if (maxBufferPtr != NULL) {
            *maxBufferPtr = itPtr->adp.bufsize;
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpIdentObjCmd --
 *
 *      Set ident string for current file.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpIdentObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    AdpFrame *framePtr = NULL;
    int       result = TCL_OK;

    if (objc != 1 && objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?/ident/?");
        result = TCL_ERROR;

    } else if (GetFrame(clientData, &framePtr) != TCL_OK) {
        result = TCL_ERROR;

    } else if (objc == 2) {
        if (framePtr->ident != NULL) {
            Tcl_DecrRefCount(framePtr->ident);
        }
        framePtr->ident = objv[1];
        Tcl_IncrRefCount(framePtr->ident);
        Tcl_SetObjResult(interp, framePtr->ident);

    } else if (framePtr->ident != NULL) {
        Tcl_SetObjResult(interp, framePtr->ident);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpCtlObjCmd --
 *
 *      ADP processing control.
 *      Implements "ns_adp_ctl".
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */

static int
AdpCtlBufSizeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK;
    Tcl_WideInt       size = -1;
    NsInterp         *itPtr = clientData;
    Ns_ObjvValueRange bufsizeRange = {1, SSIZE_MAX};
    Ns_ObjvSpec args[] = {
        {"?size", Ns_ObjvWideInt,  &size, &bufsizeRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        if (size > -1) {
            itPtr->adp.bufsize = (size_t)size;
        }
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)itPtr->adp.bufsize));
    }
    return result;
}

int
NsTclAdpCtlObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    NsInterp    *itPtr = clientData;
    Tcl_Channel  chan;
    int          opt, result = TCL_OK;
    unsigned int flag, oldFlag;

    enum {
        CBufSizeIdx = ADP_OPTIONMAX + 1u,
        CChanIdx    = ADP_OPTIONMAX + 2u
    };

    static const struct {
        const char        *option;
        const unsigned int flag;
    } adpCtlOpts[] = {
        { "bufsize",      (unsigned)CBufSizeIdx },
        { "channel",      (unsigned)CChanIdx },
        { "autoabort",    ADP_AUTOABORT },
        { "cache",        ADP_CACHE },
        { "detailerror",  ADP_DETAIL },
        { "displayerror", ADP_DISPLAY },
        { "expire",       ADP_EXPIRE },
        { "safe",         ADP_SAFE },
        { "singlescript", ADP_SINGLE },
        { "stream",       ADP_STREAM },
        { "stricterror",  ADP_STRICT },
        { "trace",        ADP_TRACE },
        { "trimspace",    ADP_TRIM },
        { NULL, 0u}
    };

    if (unlikely(objc < 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "/subcommand/ ?/arg .../?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObjStruct(interp, objv[1], adpCtlOpts,
                                         (int)sizeof(adpCtlOpts[0]),
                                         "subcommand", TCL_EXACT, &opt
                                         ) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        flag = adpCtlOpts[opt].flag;

        switch (flag) {

        case CBufSizeIdx:
            result = AdpCtlBufSizeObjCmd(clientData, interp, objc, objv);
            break;

        case CChanIdx:
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "/channel/");
                result = TCL_ERROR;
            } else {
                const char  *id = Tcl_GetString(objv[2]);

                if (*id == '\0') {
                    if (itPtr->adp.chan != NULL) {
                        if (NsAdpFlush(itPtr, NS_FALSE) != TCL_OK) {
                            result = TCL_ERROR;
                        } else {
                            itPtr->adp.chan = NULL;
                        }
                    }
                } else {
                    if (Ns_TclGetOpenChannel(interp, id, 1, NS_TRUE, &chan) != TCL_OK) {
                        result = TCL_ERROR;
                    } else {
                        itPtr->adp.chan = chan;
                    }
                }
            }
            break;

        default:
            /*
             * Query or update an ADP option.
             */

            if (objc != 2 && objc !=3 ) {
                Tcl_WrongNumArgs(interp, 2, objv, "?true|false?");
                result = TCL_ERROR;

            } else {
                oldFlag = (itPtr->adp.flags & flag);
                if (objc == 3) {
                    int boolVal;

                    if (Tcl_GetBooleanFromObj(interp, objv[2], &boolVal) != TCL_OK) {
                        result = TCL_ERROR;
                    } else {
                        if (boolVal != 0) {
                            itPtr->adp.flags |= flag;
                        } else {
                            itPtr->adp.flags &= ~flag;
                        }
                    }
                }
                if (result == TCL_OK) {
                    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(oldFlag != 0u));
                }
            }
            break;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpIncludeObjCmd --
 *
 *      Implements "_ns_adp_include" used internally to evaluate
 *      ADP pages.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      File evaluated with output going to ADP buffer.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpIncludeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char          *fileName = NULL;
    int            result, tclScript = 0, nocache = 0;
    TCL_SIZE_T     nargs = 0;
    Ns_Time       *ttlPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-cache",       Ns_ObjvTime,   &ttlPtr,    NULL},
        {"-nocache",     Ns_ObjvBool,   &nocache,   INT2PTR(NS_TRUE)},
        {"-tcl",         Ns_ObjvBool,   &tclScript, INT2PTR(NS_TRUE)},
        {"--",           Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"fileName", Ns_ObjvString, &fileName,  NULL},
        {"?arg",     Ns_ObjvArgs,   &nargs,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        NsInterp      *itPtr = clientData;
        unsigned int   flags;
        Tcl_DString   *dsPtr;

        objv = objv + (objc - (TCL_SIZE_T)nargs);
        objc = (TCL_SIZE_T)nargs;

        flags = itPtr->adp.flags;
        if (nocache != 0) {
            itPtr->adp.flags &= ~ADP_CACHE;
        }
        if (tclScript != 0) {
            itPtr->adp.flags |= ADP_TCLFILE;
        }

        /*
         * In cache refresh mode, append include command to the output
         * buffer. It will be compiled into the cached result.
         */

        if (nocache != 0 && itPtr->adp.refresh > 0) {
            if (GetOutput(clientData, &dsPtr) != TCL_OK) {
                result = TCL_ERROR;

            } else {
                TCL_SIZE_T i;

                Tcl_DStringAppend(dsPtr, "<% ns_adp_include", TCL_INDEX_NONE);
                if ((itPtr->adp.flags & ADP_TCLFILE) != 0u) {
                    Tcl_DStringAppendElement(dsPtr, "-tcl");
                }
                for (i = 0; i < objc; ++i) {
                    Tcl_DStringAppendElement(dsPtr, Tcl_GetString(objv[i]));
                }
                Tcl_DStringAppend(dsPtr, "%>", 2);
                result = TCL_OK;
            }
        } else {
            result = NsAdpInclude(clientData, objc, objv, fileName, ttlPtr);
            itPtr->adp.flags = flags;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpParseObjCmd --
 *
 *      Implements "ns_adp_parse". The command evaluates strings or ADP files
 *      at the current call frame level.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      ADP string or file output is return as Tcl result.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpParseObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result;
    TCL_SIZE_T  nargs = 0;
    int         asFile = (int)NS_FALSE, safe = (int)NS_FALSE, asString = (int)NS_FALSE, tclScript = (int)NS_FALSE;
    char       *cwd = NULL;
    Ns_ObjvSpec opts[] = {
        {"-cwd",         Ns_ObjvString, &cwd,       NULL},
        {"-file",        Ns_ObjvBool,   &asFile,    INT2PTR(NS_TRUE)},
        {"-safe",        Ns_ObjvBool,   &safe,      INT2PTR(NS_TRUE)},
        {"-string",      Ns_ObjvBool,   &asString,  INT2PTR(NS_TRUE)},
        {"-tcl",         Ns_ObjvBool,   &tclScript, INT2PTR(NS_TRUE)},
        {"--",           Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"arg", Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        objv = objv + (objc - (TCL_SIZE_T)nargs);
        objc = (TCL_SIZE_T)nargs;

        if (asString && asFile) {
            Ns_TclPrintfResult(interp, "specify either '-string' or '-file', but not both.");
            result = TCL_ERROR;

        } else {
            NsInterp    *itPtr = clientData;
            unsigned int savedFlags;
            const char  *savedCwd = NULL, *resvar = NULL;

            savedFlags = itPtr->adp.flags;

            /*
             * We control the following three flags via parameter for this
             * function, so clear the values first.
             */
            itPtr->adp.flags &= ~(ADP_TCLFILE|ADP_ADPFILE|ADP_SAFE);

            if (asFile == (int)NS_TRUE) {
                /* file mode */
                itPtr->adp.flags |= ADP_ADPFILE;
            } else {
                /* string mode */
            }
            if (tclScript == (int)NS_TRUE) {
                /* Tcl script */
                itPtr->adp.flags |= ADP_TCLFILE;
            }
            if (safe == (int)NS_TRUE) {
                itPtr->adp.flags |= ADP_SAFE;
            }

            /*
             * Check the ADP field in the nsInterp, and construct any support
             * Also, set the cwd.
             */

            if (cwd != NULL) {
                savedCwd = itPtr->adp.cwd;
                itPtr->adp.cwd = cwd;
            }
            if (asFile == (int)NS_TRUE) {
                result = NsAdpSource(clientData, objc, objv, resvar);
            } else {
                result = NsAdpEval(clientData, objc, objv, resvar);
            }
            if (cwd != NULL) {
                itPtr->adp.cwd = savedCwd;
            }
            itPtr->adp.flags = savedFlags;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpAppendObjCmd, NsTclAdpPutsObjCmd --
 *
 *      Implements "ns_adp_append" and "ns_adp_puts". Both commands append
 *      output.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Output buffer is extended with given text, including a newline
 *      with ns_adp_puts.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpAppendObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    NsInterp *itPtr = clientData;
    int       result = TCL_OK;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/string/ ?/string/ ...?");
        result = TCL_ERROR;
    } else {
        TCL_SIZE_T i;

        for (i = 1; i < objc; ++i) {
            TCL_SIZE_T  len;
            const char *s = Tcl_GetStringFromObj(objv[i], &len);

            if (NsAdpAppend(itPtr, s, len) != TCL_OK) {
                result = TCL_ERROR;
                break;
            }
        }
    }
    return result;
}

int
NsTclAdpPutsObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    NsInterp   *itPtr = clientData;
    char       *chars = NULL;
    int         nonewline = 0, result = TCL_OK;
    TCL_SIZE_T  length = 0;
    Ns_ObjvSpec opts[] = {
        {"-nonewline", Ns_ObjvBool,  &nonewline, INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"string",  Ns_ObjvString, &chars, &length},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (NsAdpAppend(itPtr, chars, length) != TCL_OK) {
        result = TCL_ERROR;

    } else if (nonewline == 0 && NsAdpAppend(itPtr, "\n", 1) != TCL_OK) {
        result = TCL_ERROR;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpDirObjCmd --
 *
 *      Implements "ns_adp_dir". This command returns the current ADP
 *      directory.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpDirObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    int             status = TCL_OK;

    if (unlikely(objc != 1)) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        status = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(itPtr->adp.cwd, TCL_INDEX_NONE));
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpReturnObjCmd, NsTclAdpBreakObjCmd, NsTclAdpAbortObjCmd --
 *
 *      Implements "ns_adp_return", "ns_adp_break" and "ns_adp_abort".
 *      These commands halt page generation.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Break or abort exception is noted and will be handled in
 *      AdpProc.
 *
 *----------------------------------------------------------------------
 */


int
NsTclAdpReturnObjCmd(ClientData clientData, Tcl_Interp *UNUSED(interp), TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return ExceptionObjCmd(clientData, objc, objv, ADP_RETURN);
}

int
NsTclAdpBreakObjCmd(ClientData clientData, Tcl_Interp *UNUSED(interp), TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return ExceptionObjCmd(clientData, objc, objv, ADP_BREAK);
}

int
NsTclAdpAbortObjCmd(ClientData clientData, Tcl_Interp *UNUSED(interp), TCL_SIZE_T objc,  Tcl_Obj *const* objv)
{
    return ExceptionObjCmd(clientData, objc, objv, ADP_ABORT);
}

static int
ExceptionObjCmd(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, AdpResult exception)
{
    Tcl_Obj     *retValObj = NULL;
    Ns_ObjvSpec  args[] = {
        {"?retval",  Ns_ObjvObj, &retValObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);

    if (Ns_ParseObjv(NULL, args, itPtr->interp, 1, objc, objv) != NS_OK) {
        /*
         * We return always TCL_ERROR
         */
    } else {
        itPtr->adp.exception = exception;
        if (retValObj != NULL) {
            Tcl_SetObjResult(itPtr->interp, retValObj);
        }
    }
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpTellObjCmd --
 *
 *      Implements "ns_adp_tell". This commands returns the current
 *      offset within the output buffer.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpTellObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_DString *dsPtr;
    int          result;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        result = TCL_ERROR;

    } else if (GetOutput(clientData, &dsPtr) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(dsPtr->length));
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpTruncObjCmd --
 *
 *      Implements "ns_adp_trunc". This commands truncates the output
 *      buffer to the given length.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Output buffer is truncated.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpTruncObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_DString      *dsPtr;
    int               result = TCL_OK;
    Tcl_WideInt       length = 0;
    Ns_ObjvSpec  args[] = {
        {"?length",  Ns_ObjvWideInt, &length, &posintRange0},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        if (GetOutput(clientData, &dsPtr) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            Ns_DStringSetLength(dsPtr, (TCL_SIZE_T)length);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpDumpObjCmd --
 *
 *      Implements "ns_adp_dump". The command returns the entire text
 *      of the output buffer.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpDumpObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_DString *dsPtr;
    int          result;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        result = TCL_ERROR;

    } else if (GetOutput(clientData, &dsPtr) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DStringResult(interp, dsPtr);
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpInfoObjCmd --
 *
 *      Implements "ns_adp_info". This command provides introspection for the
 *      current filename, size and modification time.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpInfoObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    AdpFrame *framePtr = NULL;
    int       result;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        result = TCL_ERROR;

    } else if (GetFrame(clientData, &framePtr) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_Obj  * resultObj = Tcl_NewListObj(0, NULL);

        result = Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(framePtr->file, TCL_INDEX_NONE));
        if (likely(result == TCL_OK)) {
            result = Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewWideIntObj((Tcl_WideInt)framePtr->size));
        }
        if (likely(result == TCL_OK)) {
            result = Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewWideIntObj(framePtr->mtime));
        }
        if (likely(result == TCL_OK)) {
            Tcl_SetObjResult(interp, resultObj);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpArgcObjCmd --
 *
 *      Implements "ns_adp_args". This command returns the number of
 *      arguments in the current ADP frame.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpArgcObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    AdpFrame *framePtr = NULL;
    int       result;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        result = TCL_ERROR;

    } else if (GetFrame(clientData, &framePtr) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj((int)framePtr->objc));
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpArgvObjCmd --
 *
 *      Implements "ns_adp_argv". The command returns an argument (or
 *      the entire list of arguments) within the current ADP frame.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpArgvObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    AdpFrame         *framePtr = NULL;
    Tcl_Obj          *defaultObj = NULL;
    int               result = TCL_OK;
    Tcl_WideInt       idx = 0;
    Ns_ObjvSpec       args[] = {
        {"?index",   Ns_ObjvWideInt, &idx,   &posintRange0},
        {"?default", Ns_ObjvObj, &defaultObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (GetFrame(clientData, &framePtr) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        if (objc == 1) {
            Tcl_SetListObj(Tcl_GetObjResult(interp), (TCL_SIZE_T)framePtr->objc, framePtr->objv);
        } else {
            if ((idx + 1) <= (int)framePtr->objc) {
                Tcl_SetObjResult(interp, framePtr->objv[idx]);
            } else if (defaultObj != NULL) {
                Tcl_SetObjResult(interp, defaultObj);
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpBindArgsObjCmd --
 *
 *      Implements "ns_adp_bind_args". The command is used to copy arguments
 *      from the current frame into local variables.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      One or more local variables are created.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpBindArgsObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    AdpFrame *framePtr = NULL;
    int       result = TCL_OK;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/varName/ ?/varName/ ...?");
        result = TCL_ERROR;

    } else if (GetFrame(clientData, &framePtr) != TCL_OK) {
        result = TCL_ERROR;

    } else if (objc != framePtr->objc) {
        Ns_TclPrintfResult(interp, "invalid #variables");
        result = TCL_ERROR;

    } else {
        TCL_SIZE_T i;

        for (i = 1; i < objc; ++i) {
            if (Tcl_ObjSetVar2(interp, objv[i], NULL, framePtr->objv[i],
                               TCL_LEAVE_ERR_MSG) == NULL) {
                result = TCL_ERROR;
                break;
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpExceptionObjCmd --
 *
 *      Implements "ns_adp_exception". The command returns the current
 *      exception state, ok, abort, or break.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpExceptionObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj        *varnameObj = NULL;
    int             result = TCL_OK;
    Ns_ObjvSpec     args[] = {
        {"?varName", Ns_ObjvObj, &varnameObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;

        Tcl_SetObjResult(interp, Tcl_NewBooleanObj((itPtr->adp.exception == ADP_OK)));

        if (varnameObj != NULL) {
            const char *exception = "NONE"; /* Just to make compiler silent, we have a complete enumeration of switch values */

            switch (itPtr->adp.exception) {
            case ADP_OK:
                exception = "ok";
                break;
            case ADP_BREAK:
                exception = "break";
                break;
            case ADP_ABORT:
                exception = "abort";
                break;
            case ADP_RETURN:
                exception = "return";
                break;
            case ADP_TIMEOUT:
                exception = "timeout";
                break;
            }
            if (Tcl_ObjSetVar2(interp, varnameObj, NULL, Tcl_NewStringObj(exception, TCL_INDEX_NONE),
                               TCL_LEAVE_ERR_MSG) == NULL) {
                result = TCL_ERROR;
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpFlushObjCmd, NsTclAdpCloseObjCmd  --
 *
 *      Implements "ns_adp_flush" and "ns_adp_close". Flush or close the
 *      current ADP output.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Output will flush to client immediately.
 *
 *----------------------------------------------------------------------
 */

static int
AdpFlushObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, bool doStream)
{
    int result;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        result = TCL_ERROR;
    } else {
        NsInterp *itPtr = clientData;

        result = NsAdpFlush(itPtr, doStream);
    }
    return result;
}

int
NsTclAdpFlushObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return AdpFlushObjCmd(clientData, interp, objc, objv, NS_TRUE);
}

int
NsTclAdpCloseObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return AdpFlushObjCmd(clientData, interp, objc, objv, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpDebugObjCmd --
 *
 *      Implements "ns_adp_debug". The command can be used to connect to the
 *      TclPro debugger if not already connected.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See comments for DebugInit().
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpDebugObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    NsInterp   *itPtr = clientData;
    char       *host = NULL, *port = NULL, *procs = NULL;
    int         result;
    Ns_ObjvSpec opts[] = {
        {"-host",  Ns_ObjvString, &host,  NULL},
        {"-port",  Ns_ObjvString, &port,  NULL},
        {"-procs", Ns_ObjvString, &procs, NULL},
        {"--",     Ns_ObjvBreak,  NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?host",  Ns_ObjvString, &host,  NULL},
        {"?port",  Ns_ObjvString, &port,  NULL},
        {"?procs", Ns_ObjvString, &procs, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (NsAdpDebug(itPtr, host, port, procs) != TCL_OK) {
        Ns_TclPrintfResult(interp, "could not initialize debugger");
        result = TCL_ERROR;

    } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(itPtr->adp.debugLevel));
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpMimeTypeObjCmd --
 *
 *      Implements "ns_adp_mimetype". The command can bue used to set or get
 *      the mime type returned upon completion of the parsed file.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See: Ns_ConnSetEncodedTypeHeader().
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpMimeTypeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    Ns_Conn        *conn  = itPtr->conn;
    char           *mimetypeString = NULL;
    int             result = TCL_OK;
    Ns_ObjvSpec     args[] = {
        {"?mimetype", Ns_ObjvString, &mimetypeString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (conn != NULL) {
        const char *type;

        if (mimetypeString != NULL) {
            Ns_ConnSetEncodedTypeHeader(conn, mimetypeString);
        }
        type = Ns_SetIGet(conn->outputheaders, "content-type");
        Tcl_SetObjResult(interp, Tcl_NewStringObj(type, TCL_INDEX_NONE));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GetFrame --
 *
 *      Validate and return the current execution frame.
 *
 * Results:
 *      TCL_ERROR if no active frame, TCL_OK otherwise.
 *
 * Side effects:
 *      Will update given framePtrPtr with current frame.
 *
 *----------------------------------------------------------------------
 */

static int
GetFrame(const ClientData clientData, AdpFrame **framePtrPtr)
{
    const NsInterp *itPtr;
    int             result;

    NS_NONNULL_ASSERT(clientData != NULL);
    NS_NONNULL_ASSERT(framePtrPtr != NULL);

    itPtr = clientData;
    if (itPtr->adp.framePtr == NULL) {
        Ns_TclPrintfResult(itPtr->interp, "no active adp");
        result = TCL_ERROR;
    } else {
        *framePtrPtr = itPtr->adp.framePtr;
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GetOutput --
 *
 *      Validates and returns current output buffer.
 *
 * Results:
 *      TCL_ERROR if GetFrame fails, TCL_OK otherwise.
 *
 * Side effects:
 *      Will update given dsPtrPtr with buffer.
 *
 *----------------------------------------------------------------------
 */

static int
GetOutput(ClientData clientData, Tcl_DString **dsPtrPtr)
{
    AdpFrame *framePtr = NULL;
    int       result;

    NS_NONNULL_ASSERT(clientData != NULL);
    NS_NONNULL_ASSERT(dsPtrPtr != NULL);

    if (GetFrame(clientData, &framePtr) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        *dsPtrPtr = framePtr->outputPtr;
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GetInterp --
 *
 *      Get the NsInterp structure..
 *
 * Results:
 *      TCL_ERROR if not a NaviServer interp, TCL_OK otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
GetInterp(Tcl_Interp *interp, NsInterp **itPtrPtr)
{
    NsInterp *itPtr;
    int       result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(itPtrPtr != NULL);

    itPtr = NsGetInterpData(interp);
    if (itPtr == NULL) {
        Ns_TclPrintfResult(interp, "not a server interp");
        result = TCL_ERROR;
    } else {
        *itPtrPtr = itPtr;
        result = TCL_OK;
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
