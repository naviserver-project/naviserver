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
 * adpcmds.c --
 *
 *      ADP commands.
 */

#include "nsd.h"

/*
 * Local functions defined in this file.
 */

static int ExceptionObjCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST objv[],
                           int exception);
static int EvalObjCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST objv[]);
static int GetFrame(ClientData arg, AdpFrame **framePtrPtr);
static int GetOutput(ClientData arg, Tcl_DString **dsPtrPtr);
static int GetInterp(Tcl_Interp *interp, NsInterp **itPtrPtr);



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
Ns_AdpAppend(Tcl_Interp *interp, CONST char *buf, int len)
{
    NsInterp *itPtr;

    if (GetInterp(interp, &itPtr) != TCL_OK) {
        return TCL_ERROR;
    }
    return NsAdpAppend(itPtr, buf, len);
}

int
NsAdpAppend(NsInterp *itPtr, CONST char *buf, int len)
{
    Tcl_DString *bufPtr;

    if (GetOutput(itPtr, &bufPtr) != TCL_OK) {
        return TCL_ERROR;
    }
    Ns_DStringNAppend(bufPtr, buf, len);
    if (
	((itPtr->adp.flags & ADP_STREAM) 
	 || (size_t)bufPtr->length > itPtr->adp.bufsize
	 ) 
	&& NsAdpFlush(itPtr, 1) != TCL_OK) {
        return TCL_ERROR;
    }
    return TCL_OK;
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
 *      streamPtr set to 1 if steaming mode active.
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
                int *streamPtr, size_t *maxBufferPtr)
{
    NsInterp *itPtr;

    if (GetInterp(interp, &itPtr) != TCL_OK
            || GetOutput(itPtr, dsPtrPtr) != TCL_OK) {
        return TCL_ERROR;
    }
    if (streamPtr != NULL) {
        *streamPtr = (itPtr->adp.flags & ADP_STREAM) ? 1 : 0;
    }
    if (maxBufferPtr != NULL) {
        *maxBufferPtr = itPtr->adp.bufsize;
    }

    return TCL_OK;
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
NsTclAdpIdentObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    AdpFrame *framePtr = NULL;

    if (objc != 1 && objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?ident?");
        return TCL_ERROR;
    }
    if (GetFrame(arg, &framePtr) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 2) {
        if (framePtr->ident != NULL) {
            Tcl_DecrRefCount(framePtr->ident);
        }
        framePtr->ident = objv[1];
        Tcl_IncrRefCount(framePtr->ident);
    }
    if (framePtr->ident != NULL) {
        Tcl_SetObjResult(interp, framePtr->ident);
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpCtlObjCmd --
 *
 *      ADP processing control.
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
NsTclAdpCtlObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp    *itPtr = arg;
    Tcl_Channel  chan;
    char        *id;
    size_t       size;
    int          opt;
    unsigned int flag, oldFlag;

    enum {
        CBufSizeIdx = ADP_OPTIONMAX + 1,
        CChanIdx    = ADP_OPTIONMAX + 2
    };

    static struct {
        char        *option;
        unsigned int flag;
    } adpCtlOpts[] = {

        { "bufsize",      CBufSizeIdx },
        { "channel",      CChanIdx },

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
        { NULL, 0}
    };


    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObjStruct(interp, objv[1], adpCtlOpts,
                                  sizeof(adpCtlOpts[0]), "option",
                                  TCL_EXACT, &opt) != TCL_OK) {
        return TCL_ERROR;
    }
    flag = adpCtlOpts[opt].flag;

    switch (flag) {

    case CBufSizeIdx:
        if (objc != 2 && objc !=3 ) {
            Tcl_WrongNumArgs(interp, 2, objv, "?size?");
            return TCL_ERROR;
        }
        size = itPtr->adp.bufsize;
        if (objc == 3) {
 	    int intVal;

            if (Tcl_GetIntFromObj(interp, objv[2], &intVal) != TCL_OK) {
                return TCL_ERROR;
            }
            if (intVal < 0) {
                intVal = 0;
            }
            itPtr->adp.bufsize = intVal;
        }
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(size));
        break;

    case CChanIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "channel");
            return TCL_ERROR;
        }
        id = Tcl_GetString(objv[2]);
        if (*id == '\0') {
            if (itPtr->adp.chan != NULL) {
                if (NsAdpFlush(itPtr, 0) != TCL_OK) {
                    return TCL_ERROR;
                }
                itPtr->adp.chan = NULL;
            }
        } else {
            if (Ns_TclGetOpenChannel(interp, id, 1, 1, &chan) != TCL_OK) {
                return TCL_ERROR;
            }
            itPtr->adp.chan = chan;
        }
        break;

    default:
        /*
         * Query or update an ADP option.
         */

        if (objc != 2 && objc !=3 ) {
            Tcl_WrongNumArgs(interp, 2, objv, "?bool?");
            return TCL_ERROR;
        }
        oldFlag = (itPtr->adp.flags & flag);
        if (objc == 3) {
  	    int boolVal;

            if (Tcl_GetBooleanFromObj(interp, objv[2], &boolVal) != TCL_OK) {
                return TCL_ERROR;
            }
            if (boolVal) {
                itPtr->adp.flags |= flag;
            } else {
                itPtr->adp.flags &= ~flag;
            }
        }
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(oldFlag));
        break;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpEvalObjCmd, NsTclAdpSafeEvalObjCmd --
 *
 *      (Safe) Evaluate an ADP string.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Page string is parsed and evaluated at current Tcl level in a
 *      new ADP call frame.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpEvalObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return EvalObjCmd(arg, objc, objv);
}

int
NsTclAdpSafeEvalObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;

    itPtr->adp.flags |= ADP_SAFE;
    return EvalObjCmd(arg, objc, objv);
}

static int
EvalObjCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST objv[])
{

    if (objc < 2) {
        Tcl_WrongNumArgs(itPtr->interp, 1, objv, "page ?args ...?");
	return TCL_ERROR;
    }

    return NsAdpEval(itPtr, objc-1, objv+1, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpIncludeObjCmd --
 *
 *      Process the Tcl _ns_adp_include commands to evaluate an
 *      ADP.
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
NsTclAdpIncludeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp    *itPtr = arg;
    Tcl_DString *dsPtr;
    int          result;
    unsigned int flags;
    char        *file;
    int          tcl = 0, nocache = 0, nargs = 0;
    Ns_Time     *ttlPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-cache",       Ns_ObjvTime,   &ttlPtr,  NULL},
        {"-nocache",     Ns_ObjvBool,   &nocache, (void *) NS_TRUE},
        {"-tcl",         Ns_ObjvBool,   &tcl,     (void *) NS_TRUE},
        {"--",           Ns_ObjvBreak,  NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"file",  Ns_ObjvString, &file,  NULL},
        {"?args", Ns_ObjvArgs,   &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    objv = objv + (objc - nargs);
    objc = nargs;

    flags = itPtr->adp.flags;
    if (nocache) {
        itPtr->adp.flags &= ~ADP_CACHE;
    }
    if (tcl) {
        itPtr->adp.flags |= ADP_TCLFILE;
    }
    
    /*
     * In cache refresh mode, append include command to the output
     * buffer. It will be compiled into the cached result.
     */

    if (nocache && itPtr->adp.refresh > 0) {
        int i;
        if (GetOutput(arg, &dsPtr) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_DStringAppend(dsPtr, "<% ns_adp_include", -1);
        if (itPtr->adp.flags & ADP_TCLFILE) {
            Tcl_DStringAppendElement(dsPtr, "-tcl");
        }
        for (i = 0; i < objc; ++i) {
            Tcl_DStringAppendElement(dsPtr, Tcl_GetString(objv[i]));
        }
        Tcl_DStringAppend(dsPtr, "%>", 2);
        return TCL_OK;
    }
    result = NsAdpInclude(arg, objc, objv, file, ttlPtr);

    itPtr->adp.flags = flags;

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpParseObjCmd --
 *
 *      Process the ns_adp_parse command to evaluate strings or
 *      ADP files at the current call frame level.
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
NsTclAdpParseObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp    *itPtr = arg;
    int          result, nargs = 0;
    unsigned int savedFlags;
    char        *resvar = NULL;
    int          file = 0, safe = 0, string = 0, tcl = 0;
    char        *cwd = NULL, *savedCwd = NULL;

    Ns_ObjvSpec opts[] = {
        {"-cwd",         Ns_ObjvString, &cwd,    NULL},
        {"-file",        Ns_ObjvBool,   &file,   (void *) NS_TRUE},
        {"-safe",        Ns_ObjvBool,   &safe,   (void *) NS_TRUE},
        {"-string",      Ns_ObjvBool,   &string, (void *) NS_TRUE},
        {"-tcl",         Ns_ObjvBool,   &tcl,    (void *) NS_TRUE},
        {"--",           Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"args", Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    objv = objv + (objc - nargs);
    objc = nargs;

    if (string && file) {
      Tcl_AppendResult(interp, "specify either '-string' or '-file', but not both.", NULL);
      return TCL_ERROR;
    }

    savedFlags = itPtr->adp.flags;

    /*
     * We control the following three flags via parameter for this
     * function, so clear the values first.
     */
    itPtr->adp.flags &= ~(ADP_TCLFILE|ADP_ADPFILE|ADP_SAFE);

    if (file) {
	/* file mode */
        itPtr->adp.flags |= ADP_ADPFILE;
    } else {
	/* string mode */
    }
    if (tcl) {
        /* tcl script */
        itPtr->adp.flags |= ADP_TCLFILE;
    }
    if (safe) {
        itPtr->adp.flags |= ADP_SAFE;
    }

    /*
     * Check the adp field in the nsInterp, and construct any support
     * Also, set the cwd.
     */

    if (cwd != NULL) {
        savedCwd = itPtr->adp.cwd;
        itPtr->adp.cwd = cwd;
    }
    if (file) {
        result = NsAdpSource(arg, objc, objv, resvar);
    } else {
        result = NsAdpEval(arg, objc, objv, resvar);
    }
    if (cwd != NULL) {
        itPtr->adp.cwd = savedCwd;
    }
    itPtr->adp.flags = savedFlags;

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpAppendObjCmd, NsTclAdpPutsObjCmd --
 *
 *      Process the ns_adp_append and ns_adp_puts commands to append
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
NsTclAdpAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    int       i, len;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string ?string ...?");
        return TCL_ERROR;
    }
    for (i = 1; i < objc; ++i) {
        char  *s = Tcl_GetStringFromObj(objv[i], &len);

        if (NsAdpAppend(itPtr, s, len) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    return TCL_OK;
}

int
NsTclAdpPutsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    char     *string;
    int       length, nonewline = 0;

    Ns_ObjvSpec opts[] = {
        {"-nonewline", Ns_ObjvBool,  &nonewline, (void *) NS_TRUE},
        {"--",         Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"string",  Ns_ObjvString, &string, &length},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (NsAdpAppend(itPtr, string, length) != TCL_OK) {
        return TCL_ERROR;
    }
    if (!nonewline && NsAdpAppend(itPtr, "\n", 1) != TCL_OK) {
        return TCL_ERROR;
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpDirObjCmd --
 *
 *      Process the Tcl ns_adp_dir command to return the current ADP
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
NsTclAdpDirObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    Tcl_SetResult(interp, itPtr->adp.cwd, TCL_VOLATILE);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpReturnObjCmd, NsTclAdpBreakObjCmd, NsTclAdpAbortObjCmd --
 *
 *      Process the Tcl ns_adp_return, ns_adp_break and ns_adp_abort
 *      commands to halt page generation.
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
NsTclAdpReturnObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return ExceptionObjCmd(arg, objc, objv, ADP_RETURN);
}

int
NsTclAdpBreakObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return ExceptionObjCmd(arg, objc, objv, ADP_BREAK);
}

int
NsTclAdpAbortObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    return ExceptionObjCmd(arg, objc, objv, ADP_ABORT);
}

static int
ExceptionObjCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST objv[], int exception)
{
    if (objc != 1 && objc != 2) {
        Tcl_WrongNumArgs(itPtr->interp, 1, objv, "?retval?");
        return TCL_ERROR;
    }
    itPtr->adp.exception = exception;
    if (objc == 2) {
        Tcl_SetObjResult(itPtr->interp, objv[1]);
    }
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpTellObjCmd --
 *
 *      Process the Tcl ns_adp_tell commands to return the current
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
NsTclAdpTellObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Tcl_DString *dsPtr;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    if (GetOutput(arg, &dsPtr) != TCL_OK) {
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(dsPtr->length));

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpTruncObjCmd --
 *
 *      Process the Tcl ns_adp_trunc commands to truncate the output
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
NsTclAdpTruncObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    Tcl_DString *dsPtr;
    int          length;

    if (objc != 1 && objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?length?");
        return TCL_ERROR;
    }
    if (objc == 1) {
        length = 0;
    } else {
        if (Tcl_GetIntFromObj(interp, objv[1], &length) != TCL_OK) {
            return TCL_ERROR;
        }
        if (length < 0) {
            Tcl_AppendResult(interp, "invalid length: ",
                             Tcl_GetString(objv[1]), NULL);
            return TCL_ERROR;
        }
    }
    if (GetOutput(arg, &dsPtr) != TCL_OK) {
        return TCL_ERROR;
    }
    Ns_DStringTrunc(dsPtr, length);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpDumpObjCmd --
 *
 *      Process the Tcl ns_adp_dump command to return the entire text
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
NsTclAdpDumpObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    Tcl_DString *dsPtr;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    if (GetOutput(arg, &dsPtr) != TCL_OK) {
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(dsPtr->string, dsPtr->length));

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpInfoObjCmd --
 *
 *      Process the Tcl ns_adp_info commands to return the current file name,
 *      size and modification time
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
NsTclAdpInfoObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    AdpFrame *framePtr = NULL;
    Tcl_Obj  *result;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    if (GetFrame(arg, &framePtr) != TCL_OK) {
        return TCL_ERROR;
    }
    result = Tcl_NewListObj(0, NULL);
    Tcl_ListObjAppendElement(interp, result, Tcl_NewStringObj(framePtr->file, -1));
    Tcl_ListObjAppendElement(interp, result, Tcl_NewWideIntObj(framePtr->size));
    Tcl_ListObjAppendElement(interp, result, Tcl_NewWideIntObj(framePtr->mtime));
    Tcl_SetObjResult(interp, result);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpArgcObjCmd --
 *
 *      Process the Tcl ns_adp_args commands to return the number of
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
NsTclAdpArgcObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    AdpFrame *framePtr = NULL;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    if (GetFrame(arg, &framePtr) != TCL_OK) {
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(framePtr->objc));

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpArgvObjCmd --
 *
 *      Process the Tcl ns_adp_argv command to return an argument (or
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
NsTclAdpArgvObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    AdpFrame *framePtr = NULL;
    int       i;

    if (objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?index? ?default?");
        return TCL_ERROR;
    }
    if (GetFrame(arg, &framePtr) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 1) {
        Tcl_SetListObj(Tcl_GetObjResult(interp), framePtr->objc,
                       framePtr->objv);
    } else {
        if (Tcl_GetIntFromObj(interp, objv[1], &i) != TCL_OK) {
            return TCL_ERROR;
        }
        if ((i + 1) <= framePtr->objc) {
            Tcl_SetObjResult(interp, framePtr->objv[i]);
        } else {
            if (objc == 3) {
                Tcl_SetObjResult(interp, objv[2]);
            }
        }
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpBindArgsObjCmd --
 *
 *      Process the Tcl ns_adp_bind_args commands to copy arguments
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
NsTclAdpBindArgsObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    AdpFrame *framePtr = NULL;
    int       i;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "varName ?varName ...?");
        return TCL_ERROR;
    }
    if (GetFrame(arg, &framePtr) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc != framePtr->objc) {
        Tcl_AppendResult(interp, "invalid #variables", NULL);
        return TCL_ERROR;
    }
    for (i = 1; i < objc; ++i) {
        if (Tcl_ObjSetVar2(interp, objv[i], NULL, framePtr->objv[i],
                           TCL_LEAVE_ERR_MSG) == NULL) {
            return TCL_ERROR;
        }
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpExcepetionObjCmd --
 *
 *      Process the Tcl ns_adp_exception commands to return the current
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
NsTclAdpExceptionObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    int       boolValue;

    if (objc != 1 && objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?varName?");
        return TCL_ERROR;
    }
    if (itPtr->adp.exception == ADP_OK) {
        boolValue = 0;
    } else {
        boolValue = 1;
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(boolValue));

    if (objc == 2) {
	char   *exception;
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
        default:
            exception = "unknown";
            break;
        }
        if (Tcl_ObjSetVar2(interp, objv[1], NULL, Tcl_NewStringObj(exception, -1),
                           TCL_LEAVE_ERR_MSG) == NULL) {
            return TCL_ERROR;
        }
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpFlushObjCmd, NsTclAdpCloseObjCmd  --
 *
 *      Flush or close the current ADP output.
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
AdpFlushObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[],
               int stream)
{
    NsInterp *itPtr = arg;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    return NsAdpFlush(itPtr, stream);
}

int
NsTclAdpFlushObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return AdpFlushObjCmd(arg, interp, objc, objv, 1);
}

int
NsTclAdpCloseObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    return AdpFlushObjCmd(arg, interp, objc, objv, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpDebugObjCmd --
 *
 *      Process the Tcl ns_adp_debug command to connect to the TclPro
 *      debugger if not already connected.
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
NsTclAdpDebugObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    char     *host = NULL, *port = NULL, *procs = NULL;

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
        return TCL_ERROR;
    }

    if (NsAdpDebug(itPtr, host, port, procs) != TCL_OK) {
        Tcl_SetResult(interp, "could not initialize debugger", TCL_STATIC);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(itPtr->adp.debugLevel));

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpMimeTypeCmd --
 *
 *      Process the ns_adp_mimetype command to set or get the mime type
 *      returned upon completion of the parsed file.
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
NsTclAdpMimeTypeObjCmd(ClientData arg, Tcl_Interp *interp, int objc,  Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    Ns_Conn  *conn  = itPtr->conn;

    if (objc != 1 && objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?mimetype?");
        return TCL_ERROR;
    }
    if (conn != NULL) {
	char *type;
        if (objc == 2) {
            Ns_ConnSetEncodedTypeHeader(conn, Tcl_GetString(objv[1]));
        }
        type = Ns_SetIGet(conn->outputheaders, "Content-Type");
        Tcl_SetResult(interp, type, TCL_VOLATILE);
    }
    return TCL_OK;
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
GetFrame(ClientData arg, AdpFrame **framePtrPtr)
{
    NsInterp *itPtr = arg;

    if (itPtr->adp.framePtr == NULL) {
        Tcl_SetResult(itPtr->interp, "no active adp", TCL_STATIC);
        return TCL_ERROR;
    }
    *framePtrPtr = itPtr->adp.framePtr;

    return TCL_OK;
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
GetOutput(ClientData arg, Tcl_DString **dsPtrPtr)
{
    AdpFrame *framePtr = NULL;

    if (GetFrame(arg, &framePtr) != TCL_OK) {
        return TCL_ERROR;
    }
    *dsPtrPtr = framePtr->outputPtr;

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * GetInterp --
 *
 *      Get the NsInterp structure..
 *
 * Results:
 *      TCL_ERROR if not a naviserver interp, TCL_OK otherwise.
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

    itPtr = NsGetInterpData(interp);
    if (itPtr == NULL) {
        Tcl_SetResult(interp, "not a server interp", TCL_STATIC);
        return TCL_ERROR;
    }
    *itPtrPtr = itPtr;

    return TCL_OK;
}
