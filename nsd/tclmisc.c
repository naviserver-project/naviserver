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
 * tclmisc.c --
 *
 *      Implements a lot of Tcl API commands.
 */

#include "nsd.h"

/*
 * Local functions defined in this file
 */

static TCL_OBJCMDPROC_T   IpMatchObjCmd;
static TCL_OBJCMDPROC_T   IpPropertiesObjCmd;
static TCL_OBJCMDPROC_T   IpPublicObjCmd;
static TCL_OBJCMDPROC_T   IpTrustedObjCmd;
static TCL_OBJCMDPROC_T   IpValidObjCmd;

static void SHAByteSwap(uint32_t *dest, const uint8_t *src, unsigned int words)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void SHATransform(Ns_CtxSHA1 *sha)
    NS_GNUC_NONNULL(1);
static void MD5Transform(uint32_t buf[4], const uint32_t block[16])
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int Base64EncodeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, int encoding);
static int Base64DecodeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, int encoding);

static void FinishElement(Tcl_DString *elemPtr, Tcl_DString *colsPtr, bool quoted)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);



/*
 *----------------------------------------------------------------------
 *
 * Ns_TclPrintfResult --
 *
 *      Leave a formatted message in the given Tcl interps result.
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
Ns_TclPrintfResult(Tcl_Interp *interp, const char *fmt, ...)
{
    va_list     ap;
    Tcl_DString ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(fmt != NULL);

    Tcl_DStringInit(&ds);
    va_start(ap, fmt);
    Ns_DStringVPrintf(&ds, fmt, ap);
    va_end(ap);
    Tcl_DStringResult(interp, &ds);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRunOnceObjCmd --
 *
 *      Implements "ns_runonce".  Run the given script only once.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Depends on script.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRunOnceObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *script = NULL;
    int         global = (int)NS_FALSE, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-global", Ns_ObjvBool,  &global, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"script", Ns_ObjvString, &script, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp       *itPtr = clientData;
        int                   isNew;
        static Tcl_HashTable  runTable;
        static bool           initialized = NS_FALSE;

        Ns_MasterLock();
        if (!initialized) {
            Tcl_InitHashTable(&runTable, TCL_STRING_KEYS);
            initialized = NS_TRUE;
        }
        (void) Tcl_CreateHashEntry((global != (int)NS_FALSE) ? &runTable :
                                   &itPtr->servPtr->tcl.runTable, script, &isNew);
        Ns_MasterUnlock();

        if (isNew != 0) {
            result = Tcl_Eval(interp, script);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogErrorInfo --
 *
 *      Log the global errorInfo variable to the server log along with
 *      some connection info, if available.
 *
 * Results:
 *      Returns a read-only pointer to the complete errorInfo.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_TclLogErrorInfo(Tcl_Interp *interp, const char *extraInfo)
{
    const NsInterp *itPtr = NsGetInterpData(interp);
    const char     *errorInfo, *const*logHeaders;
    Tcl_DString     ds;

    if (extraInfo != NULL) {
        Tcl_AddObjErrorInfo(interp, extraInfo, TCL_INDEX_NONE);
    }
    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    if (errorInfo == NULL) {
        errorInfo = NS_EMPTY_STRING;
    }
    if (itPtr != NULL && itPtr->conn != NULL) {
        const Ns_Conn *conn = itPtr->conn;

        Ns_DStringInit(&ds);
        if (conn->request.method != NULL) {
            Ns_DStringVarAppend(&ds, conn->request.method, " ", (char *)0L);
        }
        if (conn->request.url != NULL) {
            Ns_DStringVarAppend(&ds, conn->request.url, ", ", (char *)0L);
        }
        Ns_DStringVarAppend(&ds, "PeerAddress: ", Ns_ConnPeerAddr(conn), (char *)0L);

        logHeaders = itPtr->servPtr->tcl.errorLogHeaders;
        if (logHeaders != NULL) {
            const char *const *hdr;

            for (hdr = logHeaders; *hdr != NULL; hdr++) {
                const char *value = Ns_SetIGet(conn->headers, *hdr);

                if (value != NULL) {
                    Ns_DStringVarAppend(&ds, ", ", *hdr, ": ", value, (char *)0L);
                }
            }
        }
        Ns_Log(Error, "%s\n%s", Ns_DStringValue(&ds), errorInfo);
        Ns_DStringFree(&ds);
    } else {
        Ns_Log(Error, "%s\n%s line %d", Tcl_GetStringResult(interp), errorInfo,
               Tcl_GetErrorLine(interp));
    }

   return errorInfo;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogError --
 *
 *      Log the global errorInfo variable to the server log.
 *
 * Results:
 *      Returns a read-only pointer to the errorInfo.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_TclLogError(Tcl_Interp *interp)
{
    return Ns_TclLogErrorInfo(interp, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogErrorRequest --
 *
 *      Deprecated.  See: Ns_TclLoggErrorInfo.
 *
 * Results:
 *      Returns a pointer to the read-only errorInfo.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_TclLogErrorRequest(Tcl_Interp *interp, Ns_Conn *UNUSED(conn))
{
    return Ns_TclLogErrorInfo(interp, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogDeprecated --
 *
 *      Report that a C-implemented Tcl command is deprecated.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Write log message.
 *
 *----------------------------------------------------------------------
 */

void
Ns_LogDeprecated(Tcl_Obj *const* objv, TCL_SIZE_T objc, const char *alternative, const char *explanation)
{
    Tcl_DString ds;
    TCL_SIZE_T         i;

    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, "'", 1);
    for (i = 0; i < objc; i++) {
        const char *s;
        TCL_SIZE_T  len;

        s = Tcl_GetStringFromObj(objv[i], &len);
        Tcl_DStringAppend(&ds, s, len);
        Tcl_DStringAppend(&ds, " ", 1);
    }
    Tcl_DStringAppend(&ds, "' is deprecated. ", TCL_INDEX_NONE);
    if (alternative != NULL) {
        Tcl_DStringAppend(&ds, "Use '", TCL_INDEX_NONE);
        Tcl_DStringAppend(&ds, alternative, TCL_INDEX_NONE);
        Tcl_DStringAppend(&ds, "' instead. ", TCL_INDEX_NONE);
    }
    if (explanation != NULL) {
        Tcl_DStringAppend(&ds, explanation, TCL_INDEX_NONE);
    }
    Ns_Log(Notice, "%s", Tcl_DStringValue(&ds));
    Tcl_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetNamedVar --
 *
 *      Set a variable by denoted by a name.  Convenience routine for
 *      tcl-commands, when var names are passed in (e.g. ns_http).
 *
 * Results:
 *      NS_TRUE on success, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetNamedVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr)
{
    const Tcl_Obj *errPtr;

    Tcl_IncrRefCount(valPtr);
    errPtr = Tcl_ObjSetVar2(interp, varPtr, NULL, valPtr, TCL_LEAVE_ERR_MSG);
    Tcl_DecrRefCount(valPtr);

    return (errPtr != NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclReflowTextObjCmd --
 *
 *      Reflow a text to the specified length.
 *      Implements "ns_reflow_text".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
InsertFreshNewline(Tcl_DString *dsPtr, const char *prefixString, size_t prefixLength, size_t *outputPosPtr)
{
    if (prefixLength == 0) {
        dsPtr->string[*outputPosPtr] = '\n';
        (*outputPosPtr)++;
    } else {
        Tcl_DStringSetLength(dsPtr, dsPtr->length + (TCL_SIZE_T)prefixLength);
        dsPtr->string[*outputPosPtr] = '\n';
        (*outputPosPtr)++;
        memcpy(&dsPtr->string[*outputPosPtr], prefixString, prefixLength);
        (*outputPosPtr) += prefixLength;
    }
}


int
NsTclReflowTextObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK, lineWidth = 80, offset = 0;
    Tcl_Obj          *textObj, *prefixObj = NULL;
    Ns_ObjvValueRange widthRange = {5, INT_MAX};
    Ns_ObjvValueRange offsetRange = {0, INT_MAX};
    Ns_ObjvSpec       opts[] = {
        {"-width",  Ns_ObjvInt,     &lineWidth,  &widthRange},
        {"-offset", Ns_ObjvInt,     &offset,     &offsetRange},
        {"-prefix", Ns_ObjvObj,     &prefixObj,  NULL},
        {"--",      Ns_ObjvBreak,    NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };

    Ns_ObjvSpec  args[] = {
        {"text", Ns_ObjvObj, &textObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString ds, *dsPtr = &ds;
        size_t      k, inputPos, outputPos, currentWidth, nrPrefixes, nrNewLines = 1u;
        bool        done = NS_FALSE;
        const char *p;
        TCL_SIZE_T  textLength, prefixLength = 0;
        const char *textString = Tcl_GetStringFromObj(textObj, &textLength);
        const char *prefixString = (prefixObj == NULL ? NULL : Tcl_GetStringFromObj(prefixObj, &prefixLength));

        Tcl_DStringInit(dsPtr);

        p = textString;
        while( (p = strchr(p, INTCHAR('\n'))) != NULL) {
            nrNewLines++;
            p++;
        }

        inputPos = 0u;
        if (offset == 0 && prefixLength > 0) {
            /*
             * When we have an offset (in an incremental operation) adding a
             * prefix automatically makes little sense. When needed, the
             * prefix could be easily done on the client side.
             */
            memcpy(dsPtr->string, prefixString, prefixLength);
            outputPos = (size_t)prefixLength;
            nrPrefixes = nrNewLines;
        } else {
            outputPos = 0u;
            nrPrefixes = ((nrNewLines > 0u) ? (nrNewLines - 1) : 0u);
        }

        /*
         * Set the length of the Tcl_DString to the same size as the input
         * string plus for every linebreak+1 the prefixString.
         */
        Tcl_DStringSetLength(dsPtr, textLength + (TCL_SIZE_T)nrPrefixes * prefixLength);

        while (inputPos < (size_t)textLength && !done) {
            size_t processedPos;

            /*
             * Copy the input string until lineWidth is reached
             */
            processedPos = inputPos;
            for (currentWidth = (size_t)offset; (int)currentWidth < lineWidth; currentWidth++)  {

                if ( inputPos < (size_t)textLength) {
                    dsPtr->string[outputPos] = textString[inputPos];

                    /*
                     * In case there are newlines in the text, insert it with
                     * the prefix and reset the currentWidth. The size for of
                     * the prefix is already included in the allocated space of
                     * the string.
                     */
                    outputPos++;
                    if ( textString[inputPos] == '\n') {
                        if (prefixLength > 0) {
                            memcpy(&dsPtr->string[outputPos], prefixString, prefixLength);
                            outputPos += (size_t)prefixLength;
                        }
                        currentWidth = 0u;
                        processedPos = inputPos;
                    }
                    inputPos++;
                } else {
                    /*
                     * We reached the end of the inputString and we are done.
                     */
                    done = NS_TRUE;
                    break;
                }
            }
            offset = 0;

            if (!done) {
                bool   whitesspaceFound = NS_FALSE;
                size_t origOutputPos = outputPos;
                /*
                 * Search for the last whitespace in the input from the end
                 */
                for ( k = inputPos; k > processedPos; k--, outputPos--) {
                    if ( CHARTYPE(space, textString[k]) != 0) {
                        whitesspaceFound = NS_TRUE;
                        /*
                         * Replace the whitespace by a "\n" followed by the
                         * prefix string; we have to make sure that the dsPtr
                         * can held the additional prefix as well.
                         */
                        InsertFreshNewline(dsPtr, prefixString, (size_t)prefixLength, &outputPos);
                        /*
                         * Reset the inputPositon
                         */
                        inputPos = k + 1u;
                        break;
                    }
                }
                if (!whitesspaceFound) {
                    /*
                     * The last chunk did not include a whitespace. This
                     * happens when we find overflowing elements. In this
                     * case, let the line overflow (read forward until we
                     * find a space, and continue as usual.
                     */
                    outputPos = origOutputPos;
                    for (k = inputPos; k < (size_t)textLength; k++) {
                        if ( CHARTYPE(space, textString[k]) != 0) {
                            InsertFreshNewline(dsPtr, prefixString, (size_t)prefixLength, &outputPos);
                            inputPos++;
                            break;
                        } else {
                            dsPtr->string[outputPos] = textString[inputPos];
                            outputPos++;
                            inputPos++;
                        }
                    }
                }
            }
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclTrimObjCmd --
 *
 *      Multiline trim with optional delimiter and builtin substitution
 *      (latter is not really needed but convenient).  Trim leading spaces on
 *      multiple lines.
 *
 *      Implements "ns_trim".
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
NsTclTrimObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK, substInt = 0;
    Tcl_Obj          *textObj, *prefixObj = NULL;
    char             *delimiterString = NULL;
    Ns_ObjvSpec       opts[] = {
        {"-subst",     Ns_ObjvBool,   &substInt,        INT2PTR(NS_TRUE)},
        {"-delimiter", Ns_ObjvString, &delimiterString, NULL},
        {"-prefix",    Ns_ObjvObj,    &prefixObj,       NULL},
        {"--",         Ns_ObjvBreak,  NULL,             NULL},
        {NULL, NULL, NULL, NULL}
    };

    Ns_ObjvSpec  args[] = {
        {"text",      Ns_ObjvObj,  &textObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (delimiterString != NULL && prefixObj != NULL) {
        Ns_TclPrintfResult(interp, "invalid arguments: either -prefix or -delimiter can be specified");
        result = TCL_ERROR;

    } else if (delimiterString != NULL && strlen(delimiterString) != 1) {
        Ns_TclPrintfResult(interp, "invalid arguments: -delimiter must be a single character");
        result = TCL_ERROR;

    } else {
        Tcl_DString ds, *dsPtr = &ds;
        TCL_SIZE_T  textLength;
        char       *p;
        const char *endOfString;

        Tcl_DStringInit(dsPtr);

        if (substInt != 0) {
            textObj = Tcl_SubstObj(interp, textObj, TCL_SUBST_ALL);
        }
        p = Tcl_GetStringFromObj(textObj, &textLength);
        endOfString = p + textLength;

        if (prefixObj != NULL) {
            TCL_SIZE_T  prefixLength;
            const char *prefixString = Tcl_GetStringFromObj(prefixObj, &prefixLength);

            while(likely(p < endOfString)) {
                const char *eolString;
                char       *j;
                ptrdiff_t   length;

                if (strncmp(p, prefixString, (size_t)prefixLength) == 0) {
                    j = p + prefixLength;
                } else {
                    j = p;
                }
                eolString = strchr(j, INTCHAR('\n'));
                if (likely(eolString != NULL)) {
                    length = (eolString - j) + 1;
                } else {
                    length = (endOfString - j);
                }
                Tcl_DStringAppend(dsPtr, j, (TCL_SIZE_T)length);

                p = j + length;
            }
        } else {
            /*
             * No "-prefix"
             */
            while(likely(p < endOfString)) {
                const char *eolString;
                char       *j;
                ptrdiff_t   length;

                for (j = p; likely(j < endOfString); j++) {
                    if (CHARTYPE(space, *j) != 0) {
                        continue;
                    }
                    if (delimiterString != NULL && *j == *delimiterString) {
                        j++;
                        break;
                    }
                    break;
                }
                eolString = strchr(j, INTCHAR('\n'));
                if (likely(eolString != NULL)) {
                    length = (eolString - j) + 1;
                } else {
                    length = (endOfString - j);
                }
                Tcl_DStringAppend(dsPtr, j, (TCL_SIZE_T)length);

                p = j + length;
            }
        }
        Tcl_DStringResult(interp, dsPtr);

    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclHrefsObjCmd --
 *
 *      Implements "ns_hrefs".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclHrefsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    char        *htmlString = (char *)NS_EMPTY_STRING;
    Ns_ObjvSpec  args[] = {
        {"html", Ns_ObjvString,  &htmlString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        char       *s, *e;
        const char *p;
        Tcl_Obj    *listObj = Tcl_NewListObj(0, NULL);

        p = htmlString;
        for (;;) {
            s = strchr(p, INTCHAR('<'));
            if (s == NULL) {
                break;
            }
            e = NsParseTagEnd(s);
            if (e == NULL) {
                break;
            }
            ++s;
            *e = '\0';
            while (*s != '\0' && CHARTYPE(space, *s) != 0) {
                ++s;
            }
            if ((*s == 'a' || *s == 'A') && CHARTYPE(space, s[1]) != 0) {
                ++s;
                while (*s != '\0') {
                    if (strncasecmp(s, "href", 4u) == 0) {
                        s += 4;
                        while (*s != '\0' && CHARTYPE(space, *s) != 0) {
                            ++s;
                        }
                        if (*s == '=') {
                            char save, *he;

                            ++s;
                            while (*s != '\0' && CHARTYPE(space, *s) != 0) {
                                ++s;
                            }
                            he = NULL;
                            if (*s == '\'' || *s == '"') {
                                he = strchr(s+1, INTCHAR(*s));
                                ++s;
                            }
                            if (he == NULL) {
                                assert(s != NULL);
                                he = s;
                                while (*he != '\0' && CHARTYPE(space, *he) == 0) {
                                    ++he;
                                }
                            }
                            save = *he;
                            *he = '\0';
                            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(s, TCL_INDEX_NONE));
                            *he = save;
                            break;
                        }
                    }
                    if (*s == '\'' || *s == '\"') {
                        char quote = *s;

                        do {
                            s++;
                        } while (*s != '\0' && *s != quote);
                        continue;
                    }
                    if (*s != '\0') {
                        ++s;
                    }
                }
            }
            *e++ = '>';
            p = e;
        }
        Tcl_SetObjResult(interp, listObj);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Base64EncodeObjCmd --
 *
 *      Implements "ns_uuencode", "ns_base64encode", and "ns_base64urlencode".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

#if 0
static void hexPrint(const char *msg, const unsigned char *octets, size_t octetLength)
{
    size_t i;
    fprintf(stderr, "%s octetLength %" PRIuz ":", msg, octetLength);
    for (i = 0; i < octetLength; i++) {
        fprintf(stderr, "%.2x ", octets[i] & 0xff);
    }
    fprintf(stderr, "\n");
}
#endif

static int
Base64EncodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                   int encoding)
{
    int         result = TCL_OK, isBinary = 0;
    Tcl_Obj    *charsObj;
    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"string", Ns_ObjvObj, &charsObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        char                *buffer;
        size_t               size;
        TCL_SIZE_T           nbytes = 0;
        Tcl_DString          ds;
        const unsigned char *bytes;

        Tcl_DStringInit(&ds);
        bytes = Ns_GetBinaryString(charsObj, isBinary == 1, &nbytes, &ds);
        //hexPrint("source ", bytes,  (size_t)nbytes);

        size = (size_t)nbytes;
        buffer = ns_malloc(1u + (4u * MAX(size, 2u)) / 2u);
        (void)Ns_HtuuEncode2(bytes, size, buffer, encoding);

        Tcl_SetResult(interp, buffer, (Tcl_FreeProc *) ns_free);
        Tcl_DStringFree(&ds);
    }
    return result;
}

int
NsTclBase64EncodeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return Base64EncodeObjCmd(clientData, interp, objc, objv, 0);
}
int
NsTclBase64UrlEncodeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return Base64EncodeObjCmd(clientData, interp, objc, objv, 1);
}


/*
 *----------------------------------------------------------------------
 *
 * Base64DecodeObjCmd --
 *
 *      Implements "ns_uudecode", "ns_base64decode", and "ns_base64urldecode".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

static int
Base64DecodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                   int encoding)
{
    int      result = TCL_OK, isBinary = 0, isStrict = 0;
    Tcl_Obj *charsObj;
    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"-strict", Ns_ObjvBool, &isStrict, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"string", Ns_ObjvObj, &charsObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        TCL_SIZE_T     len;
        size_t         size;
        unsigned char *decoded;
        const char    *chars = Tcl_GetStringFromObj(charsObj, &len);

        size = (size_t)len + 3u;
        decoded = (unsigned char *)ns_malloc(size);
        result = Ns_HtuuDecode2(interp, chars, decoded, size, encoding, isStrict != 0, &size);
        //NsHexPrint("base64 decoded", decoded, size, 30, NS_FALSE);

        if (result == TCL_OK) {
            if (isBinary) {
                Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(decoded, (TCL_SIZE_T)size));

            } else {
                Tcl_DString ds, *dsPtr = &ds;

                Tcl_DStringInit(dsPtr);
                (void)Tcl_ExternalToUtfDString(NULL, (char *)decoded, (TCL_SIZE_T)size, dsPtr);
                Tcl_DStringResult(interp, dsPtr);
            }
        }
        ns_free(decoded);
    }

    return result;
}
int
NsTclBase64DecodeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return Base64DecodeObjCmd(clientData, interp, objc, objv, 0);
}
int
NsTclBase64UrlDecodeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return Base64DecodeObjCmd(clientData, interp, objc, objv, 1);
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCrashObjCmd --
 *
 *      Implements "ns_crash". Crash the server to test exception handling.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Server will segfault.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCrashObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                 TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result;

    if (Ns_ParseObjv(NULL, NULL, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        char *death;

        death = NULL;
        *death = 'x';
        result = TCL_OK;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptObjCmd --
 *
 *      Implements "ns_crypt".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCryptObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int  result = TCL_OK;
    char       *keyString;
    Tcl_Obj    *saltObj;
    Ns_ObjvSpec args[] = {
        {"key",  Ns_ObjvString, &keyString, NULL},
        {"salt", Ns_ObjvObj,    &saltObj,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        TCL_SIZE_T  saltLength;
        const char *saltString = Tcl_GetStringFromObj(saltObj, &saltLength);

        if (saltLength != 2 ) {
           Ns_TclPrintfResult(interp, "salt string must be 2 characters long");
           result = TCL_ERROR;

        } else {
            char buf[NS_ENCRYPT_BUFSIZE];

            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj(Ns_Encrypt(keyString, saltString, buf), TCL_INDEX_NONE));
       }
    }
    return result;
}

/*
 *  The SHA1 routines are borrowed from libmd:
 *
 *  * sha.c - NIST Secure Hash Algorithm, FIPS PUB 180 and 180.1.
 *  * The algorithm is by spook(s) unknown at the U.S. National Security Agency.
 *  *
 *  * Written 2 September 1992, Peter C. Gutmann.
 *  * This implementation placed in the public domain.
 *  *
 *  * Modified 1 June 1993, Colin Plumb.
 *  * Modified for the new SHS based on Peter Gutmann's work,
 *  * 18 July 1994, Colin Plumb.
 *  *
 *  * Renamed to SHA and comments updated a bit 1 November 1995, Colin Plumb.
 *  * These modifications placed in the public domain.
 *  *
 *  * Comments to pgut1@cs.aukuni.ac.nz
 *  *
 *  * Hacked for use in libmd by Martin Hinner <mhi@penguin.cz>
 *
 *  This Tcl library was hacked by Jon Salz <jsalz@mit.edu>.
 *
 */

/*
 * Define to 1 for FIPS 180.1 version (with extra rotate in prescheduling),
 * 0 for FIPS 180 version (with the mysterious "weakness" that the NSA
 * isn't talking about).
 */

#define SHA_VERSION 1

#define SHA_BLOCKBYTES 64u

/*
   Shuffle the bytes into big-endian order within words, as per the
   SHA spec.
 */


static void
SHAByteSwap(uint32_t *dest, const uint8_t *src, unsigned int words)
{
    do {
       *dest++ = (uint32_t) ((unsigned) src[0] << 8 | src[1]) << 16 |
                 ((unsigned) src[2] << 8 | src[3]);
       src += 4;
    } while (--words > 0u);
}

/*
 * Initialize the SHA values
 */
void Ns_CtxSHAInit(Ns_CtxSHA1 * ctx)
{

    /*
     * Set the h-vars to their initial values.
     */
    ctx->iv[0] = 0x67452301u;
    ctx->iv[1] = 0xEFCDAB89u;
    ctx->iv[2] = 0x98BADCFEu;
    ctx->iv[3] = 0x10325476u;
    ctx->iv[4] = 0xC3D2E1F0u;

    /*
     * Initialize bit count
     */
#if defined(HAVE_64BIT)
    ctx->bytes = 0u;
#else
    ctx->bytesHi = 0u;
    ctx->bytesLo = 0u;
#endif
}

/*
 *  The SHA f()-functions. The f1 and f3 functions can be optimized to
 *  save one boolean operation each - thanks to Rich Schroeppel,
 *  rcs@cs.arizona.edu for discovering this.
 *  The f3 function can be modified to use an addition to combine the
 *  two halves rather than OR, allowing more opportunity for using
 *  associativity in optimization. (Colin Plumb)
 */
#define f1(x, y, z) ( (z) ^ ((x) & ((y) ^ (z)) ) )         /* Rounds 0-19 */
#define f2(x, y, z) ( (x) ^ (y) ^ (z) )                    /* Rounds 20-39 */
#define f3(x, y, z) ( ((x) & (y)) + ((z) & ((x) ^ (y)) ) ) /* Rounds 40-59 */
#define f4(x, y, z) ( (x) ^ (y) ^ (z) )                    /* Rounds 60-79 */

/*
 * The SHA Mysterious Constants.
 */
#define K2  (0x5A827999u)      /* Rounds 0 -19 - floor(sqrt(2)  * 2^30) */
#define K3  (0x6ED9EBA1u)      /* Rounds 20-39 - floor(sqrt(3)  * 2^30) */
#define K5  (0x8F1BBCDCu)      /* Rounds 40-59 - floor(sqrt(5)  * 2^30) */
#define K10 (0xCA62C1D6u)      /* Rounds 60-79 - floor(sqrt(10) * 2^30) */

/*
 * 32-bit rotate left - kludged with shifts
 */
#define ROTL(n, X) ( ((X) << (n)) | ((X) >> (32-(n))) )

/*
 *  The initial expanding function
 *
 *  The hash function is defined over an 80-word expanded input array W,
 *  where the first 16 are copies of the input data, and the remaining 64
 *  are defined by W[i] = W[i-16] ^ W[i-14] ^ W[i-8] ^ W[i-3]. This
 *  implementation generates these values on the fly in a circular buffer.
 *
 *  The new "corrected" FIPS 180.1 added a 1-bit left rotate to this
 *  computation of W[i].
 *
 *  The expandx() version doesn't write the result back, which can be
 *  used for the last three rounds since those outputs are never used.
 */
#if SHA_VERSION       /* FIPS 180.1 */

#define expandx(W, i) (t = W[(i)&15u] ^ W[((i)-14)&15u] ^ W[((i)-8)&15u] ^ W[((i)-3)&15u], \
                        ROTL(1, t))
#define expand(W, i) (W[(i)&15u] = expandx(W, (i)))

#else /* Old FIPS 180 */

#define expandx(W, i) (W[(i)&15u] ^ W[((i)-14)&15u] ^ W[((i)-8)&15u] ^ W[((i)-3)&15u])
#define expand(W, i) (W[(i)&15u] ^= W[((i)-14)&15u] ^ W[((i)-8)&15u] ^ W[((i)-3)&15u])

#endif /* SHA_VERSION */

/*
   The prototype SHA sub-round

   The fundamental sub-round is
   a' = e + ROTL(5, a) + f(b, c, d) + k + data;
   b' = a;
   c' = ROTL(30, b);
   d' = c;
   e' = d;
   ... but this is implemented by unrolling the loop 5 times and renaming
   the variables (e, a, b, c, d) = (a', b', c', d', e') each iteration.
 */
#define subRound(a, b, c, d, e, f, k, data) \
    ( (e) += ROTL(5u, (a)) + f((b), (c), (d)) + (k) + (data), (b) = ROTL(30u, (b)) )
/*
 *  The above code is replicated 20 times for each of the 4 functions,
 *  using the next 20 values from the W[] array for "data" each time.
 */

/*
 *  Perform the SHA transformation. Note that this code, like MD5, seems to
 *  break some optimizing compilers due to the complexity of the expressions
 *  and the size of the basic block. It may be necessary to split it into
 *  sections, e.g. based on the four sub-rounds
 *
 *  Note that this corrupts the sha->key area.
 */

static void
SHATransform(Ns_CtxSHA1 *sha)
{
    register uint32_t A, B, C, D, E;
#if SHA_VERSION
    register uint32_t t;
#endif

    NS_NONNULL_ASSERT(sha != NULL);

    /*
     * Set up first buffer
     */
    A = sha->iv[0];
    B = sha->iv[1];
    C = sha->iv[2];
    D = sha->iv[3];
    E = sha->iv[4];

    /*
     * Heavy mangling, in 4 sub-rounds of 20 interactions each.
     */
    subRound (A, B, C, D, E, f1, K2, sha->key[0]);
    subRound (E, A, B, C, D, f1, K2, sha->key[1]);
    subRound (D, E, A, B, C, f1, K2, sha->key[2]);
    subRound (C, D, E, A, B, f1, K2, sha->key[3]);
    subRound (B, C, D, E, A, f1, K2, sha->key[4]);
    subRound (A, B, C, D, E, f1, K2, sha->key[5]);
    subRound (E, A, B, C, D, f1, K2, sha->key[6]);
    subRound (D, E, A, B, C, f1, K2, sha->key[7]);
    subRound (C, D, E, A, B, f1, K2, sha->key[8]);
    subRound (B, C, D, E, A, f1, K2, sha->key[9]);
    subRound (A, B, C, D, E, f1, K2, sha->key[10]);
    subRound (E, A, B, C, D, f1, K2, sha->key[11]);
    subRound (D, E, A, B, C, f1, K2, sha->key[12]);
    subRound (C, D, E, A, B, f1, K2, sha->key[13]);
    subRound (B, C, D, E, A, f1, K2, sha->key[14]);
    subRound (A, B, C, D, E, f1, K2, sha->key[15]);
    subRound (E, A, B, C, D, f1, K2, expand (sha->key, 16u));
    subRound (D, E, A, B, C, f1, K2, expand (sha->key, 17u));
    subRound (C, D, E, A, B, f1, K2, expand (sha->key, 18u));
    subRound (B, C, D, E, A, f1, K2, expand (sha->key, 19u));

    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 20u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 21u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 22u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 23u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 24u));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 25u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 26u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 27u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 28u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 29u));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 30u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 31u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 32u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 33u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 34u));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 35u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 36u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 37u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 38u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 39u));

    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 40u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 41u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 42u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 43u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 44u));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 45u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 46u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 47u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 48u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 49u));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 50u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 51u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 52u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 53u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 54u));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 55u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 56u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 57u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 58u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 59u));

    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 60u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 61u));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 62u));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 63u));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 64u));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 65u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 66u));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 67u));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 68u));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 69u));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 70u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 71u));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 72u));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 73u));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 74u));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 75u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 76u));
    subRound (D, E, A, B, C, f4, K10, expandx (sha->key, 77u));
    subRound (C, D, E, A, B, f4, K10, expandx (sha->key, 78u));
    subRound (B, C, D, E, A, f4, K10, expandx (sha->key, 79u));

    /*
     * Build message digest
     */
    sha->iv[0] += A;
    sha->iv[1] += B;
    sha->iv[2] += C;
    sha->iv[3] += D;
    sha->iv[4] += E;
}

/*
 * Update SHA for a block of data.
 */
void Ns_CtxSHAUpdate(Ns_CtxSHA1 *ctx, const unsigned char *buf, size_t len)
{
    unsigned i;

    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    /*
     * Update bit count
     */

#if defined(HAVE_64BIT)
    i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
    ctx->bytes += len;
#else
    {
        uint32_t t = ctx->bytesLo;
        ctx->bytesLo = (uint32_t)(t + len);
        if (ctx->bytesLo < t) {
            ctx->bytesHi++;                    /* Carry from low to high */
        }
        i = (unsigned) t % SHA_BLOCKBYTES;     /* Bytes already in ctx->key */
    }
#endif

    /*
     * "i" is always less than SHA_BLOCKBYTES.
     */
    if (SHA_BLOCKBYTES - i > len) {
        memcpy(ctx->key + i, buf, len);

    } else {
        if (i != 0u) {                         /* First chunk is an odd size */
            memcpy(ctx->key + i, buf, SHA_BLOCKBYTES - i);
            SHAByteSwap(ctx->key, (const uint8_t *) ctx->key, SHA_BLOCKWORDS);
            SHATransform(ctx);
            buf += SHA_BLOCKBYTES - i;
            len -= SHA_BLOCKBYTES - i;
        }

        /*
         * Process data in 64-byte chunks
         */
        while (len >= SHA_BLOCKBYTES) {
            SHAByteSwap(ctx->key, buf, SHA_BLOCKWORDS);
            SHATransform(ctx);
            buf += SHA_BLOCKBYTES;
            len -= SHA_BLOCKBYTES;
        }

        /*
         * Handle any remaining bytes of data.
         */
        if (len != 0u) {
            memcpy(ctx->key, buf, len);
        }
    }
}

/*
 * Final wrap-up - pad to 64-byte boundary with the bit pattern
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
void Ns_CtxSHAFinal(Ns_CtxSHA1 *ctx, unsigned char digest[20])
{
#if defined(HAVE_64BIT)
    unsigned i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
#else
    unsigned i = (unsigned) ctx->bytesLo % SHA_BLOCKBYTES;
#endif
    uint8_t *p = (uint8_t *) ctx->key + i;     /* First unused byte */

    /*
     * Set the first char of padding to 0x80. There is always room.
     */
    *p++ = (uint8_t)0x80u;

    /*
     * Bytes of padding needed to make 64 bytes (0..63)
     */
    i = (SHA_BLOCKBYTES - 1u) - i;

    if (i < 8u) {
        /*
         * Padding forces an extra block
         */
        memset(p, 0, i);
        SHAByteSwap(ctx->key, (const uint8_t *) ctx->key, 16u);
        SHATransform(ctx);
        p = (uint8_t *) ctx->key;
        i = 64u;
    }
    memset(p, 0, i - 8u);
    SHAByteSwap(ctx->key, (const uint8_t *) ctx->key, 14u);

    /*
     * Append length in bits and transform
     */
#if defined(HAVE_64BIT)
    ctx->key[14] = (uint32_t) (ctx->bytes >> 29);
    ctx->key[15] = (uint32_t) ctx->bytes << 3;
#else
    ctx->key[14] = ctx->bytesHi << 3 | ctx->bytesLo >> 29;
    ctx->key[15] = ctx->bytesLo << 3;
#endif
    SHATransform (ctx);

    /*
     * The following memcpy() does not seem to be correct and is most likely
     * not needed, since the loop sets all elements of "digetst".
     */
    /*memcpy(digest, ctx->iv, sizeof(digest));*/

    for (i = 0u; i < SHA_HASHWORDS; i++) {
        uint32_t t = ctx->iv[i];

        digest[i * 4u     ] = (uint8_t) (t >> 24);
        digest[i * 4u + 1u] = (uint8_t) (t >> 16);
        digest[i * 4u + 2u] = (uint8_t) (t >> 8);
        digest[i * 4u + 3u] = (uint8_t) t;
    }

    /*
     * In case it is sensitive
     */
    memset(ctx, 0, sizeof(Ns_CtxSHA1));
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_HexString --
 *
 *      Transform binary data to hex. The provided buffer must be
 *      at least size*2 + 1 bytes long.
 *
 * Results:
 *      buffer
 *
 * Side effects:
 *      Updates passed-in buffer (2nd argument).
 *
 *----------------------------------------------------------------------
 */
char *
Ns_HexString(const unsigned char *octets, char *outputBuffer, TCL_SIZE_T size, bool isUpper)
{
    TCL_SIZE_T i;
    static const char hexCharsUpper[] = "0123456789ABCDEF";
    static const char hexCharsLower[] = "0123456789abcdef";

    NS_NONNULL_ASSERT(octets != NULL);
    NS_NONNULL_ASSERT(outputBuffer != NULL);

    if (isUpper) {
        for (i = 0; i < size; ++i) {
            outputBuffer[i * 2] = hexCharsUpper[octets[i] >> 4];
            outputBuffer[i * 2 + 1] = hexCharsUpper[octets[i] & 0xFu];
        }
    } else {
        for (i = 0; i < size; ++i) {
            outputBuffer[i * 2] = hexCharsLower[octets[i] >> 4];
            outputBuffer[i * 2 + 1] = hexCharsLower[octets[i] & 0xFu];
        }
    }
    outputBuffer[size * 2] = '\0';

    return outputBuffer;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSHA1ObjCmd --
 *
 *      Implements "ns_sha1". Returns a 40-character, hex-encoded string
 *      containing the SHA1 hash of the first argument.
 *
 * Results:
 *      NS_OK
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSHA1ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, isBinary = 0;
    Tcl_Obj    *charsObj;
    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"string", Ns_ObjvObj, &charsObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned char  digest[20];
        char           digestChars[41];
        Ns_CtxSHA1     ctx;
        TCL_SIZE_T     nbytes;
        const unsigned char *bytes;
        Tcl_DString    ds;

        Tcl_DStringInit(&ds);
        bytes = Ns_GetBinaryString(charsObj, isBinary == 1, &nbytes, &ds);
        //hexPrint("source ", bytes, (size_t)nbytes);

        Ns_CtxSHAInit(&ctx);
        Ns_CtxSHAUpdate(&ctx, bytes, (size_t) nbytes);
        Ns_CtxSHAFinal(&ctx, digest);

        Ns_HexString(digest, digestChars, 20, NS_TRUE);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(digestChars, 40));
        Tcl_DStringFree(&ds);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclFileStatObjCmd --
 *
 *      Implements "ns_filestat". Works as "file stat" command but uses native
 *      call when Tcl VFS is not compiled. The reason for this when native
 *      calls are used for speed, having still slow file stat does not help,
 *      need to use native call and file stat is the most used command
 *
 * Results:
 *      NS_OK
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclFileStatObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *filenameString, *varnameString = NULL;
    struct stat st;
    Ns_ObjvSpec args[] = {
        {"filename", Ns_ObjvString, &filenameString, NULL},
        {"?varname", Ns_ObjvString, &varnameString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (stat(filenameString, &st) != 0) {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));

    } else {
        if (varnameString != NULL) {
            (void)Tcl_SetVar2Ex(interp, varnameString, "dev",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_dev), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "ino",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_ino), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "nlink", Tcl_NewWideIntObj((Tcl_WideInt)st.st_nlink), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "uid",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_uid), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "gid",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_gid), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "size",  Tcl_NewWideIntObj((Tcl_WideInt)st.st_size), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "atime", Tcl_NewWideIntObj((Tcl_WideInt)st.st_atime), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "ctime", Tcl_NewWideIntObj((Tcl_WideInt)st.st_ctime), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "mtime", Tcl_NewWideIntObj((Tcl_WideInt)st.st_mtime), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "mode",  Tcl_NewWideIntObj((Tcl_WideInt)st.st_mode), 0);
            (void)Tcl_SetVar2Ex(interp, varnameString, "type",  Tcl_NewStringObj(
                  (S_ISREG(st.st_mode) ? "file" :
                        S_ISDIR(st.st_mode) ? "directory" :
#ifdef S_ISCHR
                          S_ISCHR(st.st_mode) ? "characterSpecial" :
#endif
#ifdef S_ISBLK
                            S_ISBLK(st.st_mode) ? "blockSpecial" :
#endif
#ifdef S_ISFIFO
                              S_ISFIFO(st.st_mode) ? "fifo" :
#endif
#ifdef S_ISLNK
                                S_ISLNK(st.st_mode) ? "link" :
#endif
#ifdef S_ISSOCK
                                  S_ISSOCK(st.st_mode) ? "socket" :
#endif
                   NS_EMPTY_STRING), TCL_INDEX_NONE), 0);
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
    }
    return result;
}

/*
 *
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */

#ifdef sun
#define HIGHFIRST
#endif

#ifndef HIGHFIRST
#define byteReverse(buf, len)   /* Nothing */
#else
/*
 * Note: this code is harmless on little-endian machines.
 */
static void byteReverse(unsigned char *buf, unsigned longs)
{
    do {
        uint32_t t;

        t = (uint32_t)
            ((unsigned) buf[3] << 8 | buf[2]) << 16 |
            ((unsigned) buf[1] << 8 | buf[0]);
        *(uint32_t *) buf = t;
        buf += 4;
    } while (--longs);
}
#endif

/*
 * Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
void Ns_CtxMD5Init(Ns_CtxMD5 *ctx)
{
    ctx->buf[0] = 0x67452301u;
    ctx->buf[1] = 0xefcdab89u;
    ctx->buf[2] = 0x98badcfeU;
    ctx->buf[3] = 0x10325476u;

    ctx->bits[0] = 0u;
    ctx->bits[1] = 0u;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
void Ns_CtxMD5Update(Ns_CtxMD5 *ctx, const unsigned char *buf, size_t len)
{
    uint32_t t;

    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    /*
     * Update bit count.
     */
    t = ctx->bits[0];
    ctx->bits[0] = t + ((uint32_t) len << 3);
    if (ctx->bits[0] < t) {
        ctx->bits[1]++;       /* Carry from low to high */
    }
    ctx->bits[1] += (uint32_t)(len >> 29);

    t = (t >> 3) & 0x3Fu;       /* Bytes already in shsInfo->data */

    /*
     * Handle any leading odd-sized chunks
     */

    if (t != 0u) {
        unsigned char *p = ctx->in + t;

        t = 64u - t;
        if (len < t) {
            memcpy(p, buf, len);
            return;
        }
        memcpy(p, buf, t);
        byteReverse(ctx->in, 16);
        MD5Transform(ctx->buf, (uint32_t *) ctx->in);
        buf += t;
        len -= t;
    }

    /*
     * Process data in 64-byte chunks
     */
    while (len >= 64u) {
        memcpy(ctx->in, buf, 64u);
        byteReverse(ctx->in, 16);
        MD5Transform(ctx->buf, (uint32_t *) ctx->in);
        buf += 64;
        len -= 64u;
    }

    /*
     * Handle any remaining bytes of data.
     */
    memcpy(ctx->in, buf, len);
}

/*
 * Final wrap-up - pad to 64-byte boundary with the bit pattern
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
void Ns_CtxMD5Final(Ns_CtxMD5 *ctx, unsigned char digest[16])
{
    unsigned count;
    uint8_t  *p;
    uint32_t *words;

    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(digest != NULL);

    words = (uint32_t *)ctx->in;

    /*
     * Compute number of bytes mod 64
     */
    count = (ctx->bits[0] >> 3) & 0x3Fu;

    /*
     * Set the first char of padding to 0x80.  This is safe since there is
     * always at least one byte free
     */
    p = ctx->in + count;
    *p++ = (uint8_t)0x80u;

    /*
     * Bytes of padding needed to make 64 bytes
     */
    count = (64u - 1u) - count;

    /*
     * Pad out to 56 mod 64
     */
    if (count < 8u) {
        /*
         * Two lots of padding:  Pad the first block to 64 bytes
         */
        memset(p, 0, count);
        byteReverse(ctx->in, 16);
        MD5Transform(ctx->buf, (uint32_t *) ctx->in);

        /*
         * Now fill the next block with 56 bytes
         */
        memset(ctx->in, 0, 56u);
    } else {
        /*
         * Pad block to 56 bytes
         */
        memset(p, 0, count - 8u);
    }
    byteReverse(ctx->in, 14);

    /*
     * Append length in bits and transform
     */
    words[14] = ctx->bits[0];
    words[15] = ctx->bits[1];

    MD5Transform(ctx->buf, (uint32_t *) ctx->in);
    byteReverse((unsigned char *) ctx->buf, 4);
    memcpy(digest, ctx->buf, 16u);
    /*
     * This memset should not be needed, since this is performed at the end of
     * the operation. In case, it would be needed, it should be necessary at
     * the initialization of the structure.
     *
     *     memset(ctx, 0, sizeof(Ns_CtxMD5));
     */
}

/*
 * The four core functions - F1 is optimized somewhat
 */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define F2(x, y, z) (F1((z), (x), (y)))
#define F3(x, y, z) ((x) ^ (y) ^ (z))
#define F4(x, y, z) ((y) ^ ((x) | ~(z)))

/*
 * This is the central step in the MD5 algorithm.
 */
#define MD5STEP(f, w, x, y, z, data, s) \
    ( (w) += f((x), (y), (z)) + (data),  (w) = (w)<<(s) | (w)>>(32-(s)),  (w) += (x) )

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to reflect
 * the addition of 16 32-bit words (64 bytes) of new data.  MD5Update blocks the
 * data and converts bytes into longwords for this routine.
 */
static void MD5Transform(uint32_t buf[4], const uint32_t block[16])
{
    register uint32_t a, b, c, d;

#ifndef HIGHFIRST
    const uint32_t *in = block;
#else
    uint32_t in[16];

    memcpy(in, block, sizeof(in));

    for (a = 0; a < 16; a++) {
        in[a] = (uint32_t)(
                            (uint32_t)(block[a * 4 + 0]) |
                            (uint32_t)(block[a * 4 + 1]) <<  8 |
                            (uint32_t)(block[a * 4 + 2]) << 16 |
                            (uint32_t)(block[a * 4 + 3]) << 24);
    }
#endif

    NS_NONNULL_ASSERT(buf != NULL);
    NS_NONNULL_ASSERT(block != NULL);

    a = buf[0];
    b = buf[1];
    c = buf[2];
    d = buf[3];

    MD5STEP(F1, a, b, c, d,  in[0] + 0xd76aa478u,  7);
    MD5STEP(F1, d, a, b, c,  in[1] + 0xe8c7b756u, 12);
    MD5STEP(F1, c, d, a, b,  in[2] + 0x242070dbU, 17);
    MD5STEP(F1, b, c, d, a,  in[3] + 0xc1bdceeeU, 22);
    MD5STEP(F1, a, b, c, d,  in[4] + 0xf57c0fafU,  7);
    MD5STEP(F1, d, a, b, c,  in[5] + 0x4787c62aU, 12);
    MD5STEP(F1, c, d, a, b,  in[6] + 0xa8304613u, 17);
    MD5STEP(F1, b, c, d, a,  in[7] + 0xfd469501u, 22);
    MD5STEP(F1, a, b, c, d,  in[8] + 0x698098d8u,  7);
    MD5STEP(F1, d, a, b, c,  in[9] + 0x8b44f7afU, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1u, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7beU, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122u,  7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193u, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438eU, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821u, 22);

    MD5STEP(F2, a, b, c, d,  in[1] + 0xf61e2562u,  5);
    MD5STEP(F2, d, a, b, c,  in[6] + 0xc040b340u,  9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51u, 14);
    MD5STEP(F2, b, c, d, a,  in[0] + 0xe9b6c7aaU, 20);
    MD5STEP(F2, a, b, c, d,  in[5] + 0xd62f105dU,  5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453u,  9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681u, 14);
    MD5STEP(F2, b, c, d, a,  in[4] + 0xe7d3fbc8u, 20);
    MD5STEP(F2, a, b, c, d,  in[9] + 0x21e1cde6u,  5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6u,  9);
    MD5STEP(F2, c, d, a, b,  in[3] + 0xf4d50d87u, 14);
    MD5STEP(F2, b, c, d, a,  in[8] + 0x455a14edU, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905u,  5);
    MD5STEP(F2, d, a, b, c,  in[2] + 0xfcefa3f8u,  9);
    MD5STEP(F2, c, d, a, b,  in[7] + 0x676f02d9u, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8aU, 20);

    MD5STEP(F3, a, b, c, d,  in[5] + 0xfffa3942u,  4);
    MD5STEP(F3, d, a, b, c,  in[8] + 0x8771f681u, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122u, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380cU, 23);
    MD5STEP(F3, a, b, c, d,  in[1] + 0xa4beea44u,  4);
    MD5STEP(F3, d, a, b, c,  in[4] + 0x4bdecfa9u, 11);
    MD5STEP(F3, c, d, a, b,  in[7] + 0xf6bb4b60u, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70u, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6u,  4);
    MD5STEP(F3, d, a, b, c,  in[0] + 0xeaa127faU, 11);
    MD5STEP(F3, c, d, a, b,  in[3] + 0xd4ef3085u, 16);
    MD5STEP(F3, b, c, d, a,  in[6] + 0x04881d05u, 23);
    MD5STEP(F3, a, b, c, d,  in[9] + 0xd9d4d039u,  4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5u, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8u, 16);
    MD5STEP(F3, b, c, d, a,  in[2] + 0xc4ac5665u, 23);

    MD5STEP(F4, a, b, c, d,  in[0] + 0xf4292244u,  6);
    MD5STEP(F4, d, a, b, c,  in[7] + 0x432aff97u, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7u, 15);
    MD5STEP(F4, b, c, d, a,  in[5] + 0xfc93a039u, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3u,  6);
    MD5STEP(F4, d, a, b, c,  in[3] + 0x8f0ccc92u, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47dU, 15);
    MD5STEP(F4, b, c, d, a,  in[1] + 0x85845dd1u, 21);
    MD5STEP(F4, a, b, c, d,  in[8] + 0x6fa87e4fU,  6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0u, 10);
    MD5STEP(F4, c, d, a, b,  in[6] + 0xa3014314u, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1u, 21);
    MD5STEP(F4, a, b, c, d,  in[4] + 0xf7537e82u,  6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235u, 10);
    MD5STEP(F4, c, d, a, b,  in[2] + 0x2ad7d2bbU, 15);
    MD5STEP(F4, b, c, d, a,  in[9] + 0xeb86d391u, 21);

    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclMD5ObjCmd --
 *
 *      Implements "ns_md5". Returns a 32-character, hex-encoded string
 *      containing the MD5 hash of the first argument.
 *
 * Results:
 *      NS_OK
 *
 * Side effects:
 *      Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclMD5ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, isBinary = 0;
    Tcl_Obj    *charsObj;
    Ns_ObjvSpec opts[] = {
        {"-binary", Ns_ObjvBool, &isBinary, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"string", Ns_ObjvObj, &charsObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_CtxMD5            ctx;
        unsigned char        digest[16];
        char                 digestChars[33];
        TCL_SIZE_T           length;
        Tcl_DString          ds;
        const unsigned char *str;

        Tcl_DStringInit(&ds);
        str = Ns_GetBinaryString(charsObj, isBinary == 1, &length, &ds);
        //NsHexPrint("md5 input data", str, length, 30, NS_FALSE);

        Ns_CtxMD5Init(&ctx);
        Ns_CtxMD5Update(&ctx, (const unsigned char *) str, (size_t)length);
        Ns_CtxMD5Final(&ctx, digest);

        Ns_HexString(digest, digestChars, 16, NS_TRUE);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(digestChars, 32));
        Tcl_DStringFree(&ds);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclSetUserObjCmd, NsTclSetGroupObjCmd --
 *
 *      Implements "ns_setuser" and "ns_setgroup".
 *
 * Results:
 *      Standard Tcl result code.
 *
 * Side effects:
 *      Error message will be output in the log file, not returned as Tcl result
 *
 *----------------------------------------------------------------------
 */

int
NsTclSetUserObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                   TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/user/");
        result = TCL_ERROR;

    } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_SetUser(Tcl_GetString(objv[1]))));
    }

    return result;
}

int
NsTclSetGroupObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/group/");
        result = TCL_ERROR;

    } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_SetGroup(Tcl_GetString(objv[1]))));
    }
    return result;
}

#ifndef _WIN32

/*
 *----------------------------------------------------------------------
 *
 * GetLimitObj --
 *
 *      Get single resource limit in form of a Tcl_Obj
 *
 * Results:
 *      Tcl_Obj
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
GetLimitObj(rlim_t value)
{
    Tcl_Obj *obj;

    if (value == RLIM_INFINITY) {
        obj = Tcl_NewStringObj("unlimited", TCL_INDEX_NONE);
    } else {
        obj = Tcl_NewWideIntObj((Tcl_WideInt)value);
    }
    return obj;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * NsTclRlimitObjCmd --
 *
 *      Implements "ns_rlimit". Get or set a resource limit in the operating
 *      system.
 *
 * Results:
 *      Pair of actual value and maximum value
 *
 * Side effects:
 *      Change resource limit with called with a value.
 *
 *----------------------------------------------------------------------
 */
int
NsTclRlimitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
#ifndef _WIN32
# ifndef RLIMIT_AS
#  define RLIMIT_AS RLIMIT_DATA
# endif

    int            opt, result = TCL_OK, rc;
    struct rlimit  rlimit;

    static const char *const opts[] = {
        "coresize",
        "datasize",
        "files",
        "filesize",
        "vmsize",
        NULL
    };
    static int resource[] = {
        RLIMIT_CORE,
        RLIMIT_DATA,
        RLIMIT_NOFILE,
        RLIMIT_FSIZE,
        RLIMIT_AS
    };
    enum {
        CCoresizeIdx,
        CDatasizeIdx,
        CFIlesizeIdx,
        CFilesIdx,
        CVmsizeIdx,

    };

    Tcl_Obj    *valueObj = NULL;
    Ns_ObjvSpec largs[] = {
        {"?value", Ns_ObjvObj, &valueObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/subcommand/ ?/arg .../?");
        return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], opts,
                            "subcommand", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    if (Ns_ParseObjv(NULL, largs, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (valueObj == NULL) {
        rc = getrlimit(resource[opt], &rlimit);
        if (rc == -1) {
            Ns_TclPrintfResult(interp, "getrlimit returned error");
            result = TCL_ERROR;
        }
    } else {
        Tcl_WideInt value;

        result = Tcl_GetWideIntFromObj(interp, valueObj, &value);
        if (result != TCL_OK) {
            char *valueString = Tcl_GetString(valueObj);

            if (strcmp(valueString, "unlimited") == 0) {
                value = (Tcl_WideInt)RLIM_INFINITY;
                result = TCL_OK;
            }
        }
        if (result == TCL_OK) {
            rc = getrlimit(resource[opt], &rlimit);
            if (rc > -1) {
                rlimit.rlim_cur = (rlim_t)value;
                rc = setrlimit(resource[opt], &rlimit);
            }
            if (rc == -1) {
                Ns_TclPrintfResult(interp, "could not set limit");
                result = TCL_ERROR;
            }
        }
    }

    if (result == TCL_OK) {
        Tcl_Obj *listPtr = Tcl_NewListObj(0, NULL);

        Tcl_ListObjAppendElement(interp, listPtr, GetLimitObj(rlimit.rlim_cur));
        Tcl_ListObjAppendElement(interp, listPtr, GetLimitObj(rlimit.rlim_max));
        Tcl_SetObjResult(interp, listPtr);
        result = TCL_OK;
    }

    return result;
#else
    return TCL_OK;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHash --
 *
 *      Produce a numeric hash value from a given string.  This function uses
 *      the Tcl built-in hash function which is commented in Tcl as follows:
 *
 *          I tried a zillion different hash functions and asked many other
 *          people for advice. Many people had their own favorite functions,
 *          all different, but no-one had much idea why they were good ones. I
 *          chose the one below (multiply by 9 and add new character) because
 *          of the following reasons:
 *
 *          1. Multiplying by 10 is perfect for keys that are decimal strings, and
 *             multiplying by 9 is just about as good.
 *          2. Times-9 is (shift-left-3) plus (old). This means that each
 *             character's bits hang around in the low-order bits of the hash value
 *             for ever, plus they spread fairly rapidly up to the high-order bits
 *             to fill out the hash value. This seems works well both for decimal
 *             and non-decimal strings, but isn't strong against maliciously-chosen
 *             keys.
 *
 *          Note that this function is very weak against malicious strings;
 *          it is very easy to generate multiple keys that have the same
 *          hashcode. On the other hand, that hardly ever actually occurs and
 *          this function *is* very cheap, even by comparison with
 *          industry-standard hashes like FNV.
 *
 * Results:
 *      Numeric hash value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
size_t
NsTclHash(const char *inputString)
{
    size_t hashValue;

    if ((hashValue = UCHAR(*inputString)) != 0) {
        char c;

        while ((c = *++inputString) != 0) {
            hashValue += (hashValue << 3) + UCHAR(c);
        }
    }
    return hashValue;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclHashObjCmd --
 *
 *      Implements "ns_hash". Tcl function to produce numeric hash value from
 *      a given string based on the algorithm that is used in Tcl internally
 *      for hashing. This is not a secure hash, but fast.
 *
 * Results:
 *      Hash value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclHashObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    char        *inputString = (char*)"";
    Ns_ObjvSpec  args[] = {
        {"value", Ns_ObjvString,  &inputString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewLongObj((long)NsTclHash(inputString)));
    }
    return result;

}

/*
 *----------------------------------------------------------------------
 *
 * NsTclValidUtf8ObjCmd --
 *
 *      Check, if the input string is valid UTF-8.
 *
 *      Implements "ns_valid_utf8".
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
NsTclValidUtf8ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result;
    Tcl_Obj    *stringObj = NULL, *errorVarnameObj = NULL;
    Ns_ObjvSpec args[] = {
        {"string",   Ns_ObjvObj, &stringObj, NULL},
        {"?varname", Ns_ObjvObj, &errorVarnameObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_DString          stringDS, errorDS;
        TCL_SIZE_T           stringLength;
        const unsigned char *bytes;
        bool                 isValid;

        Tcl_DStringInit(&stringDS);
        bytes = Ns_GetBinaryString(stringObj, 1, &stringLength, &stringDS);
        isValid = Ns_Valid_UTF8(bytes, (size_t)stringLength, &errorDS);

        if (!isValid) {
            if (errorVarnameObj != NULL) {
                Tcl_DString outputDS;

                Tcl_DStringInit(&outputDS);
                Ns_DStringAppendPrintable(&outputDS, NS_FALSE, NS_FALSE,
                                          errorDS.string, (size_t)errorDS.length);

                Tcl_ObjSetVar2(interp, errorVarnameObj, NULL,
                               Tcl_NewStringObj(outputDS.string, outputDS.length),
                               0);
                Tcl_DStringFree(&outputDS);
            }
            Tcl_DStringFree(&errorDS);
        }

        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(isValid));
        Tcl_DStringFree(&stringDS);
        result = TCL_OK;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclBaseUnitObjCmd --
 *
 *      Convert the provided argument to its base unit
 *
 *      Implements "ns_baseunit".
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
NsTclBaseUnitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result;
    Tcl_WideInt  memUnitValue = -1;
    Ns_Time     *tPtr = NULL;
    Ns_ObjvSpec opts[] = {
        {"-size",  Ns_ObjvMemUnit, &memUnitValue, NULL},
        {"-time",  Ns_ObjvTime, &tPtr,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, NULL, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (objc != 3) {
        Ns_TclPrintfResult(interp, "either -size or -time must be specified");
        result = TCL_ERROR;

    } else {
        const char *argString = Tcl_GetString(objv[1]);

        if (argString[1] == 's') {
            Tcl_SetObjResult(interp, Tcl_NewWideIntObj(memUnitValue));
            result = TCL_OK;

        } else if (argString[1] == 't') {
            Tcl_DString ds, *dsPtr = &ds;

            Tcl_DStringInit(dsPtr);
            Ns_DStringAppendTime(dsPtr, tPtr);
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;

        } else {
            Ns_TclPrintfResult(interp, "either -size or -time must be specified");
            result = TCL_ERROR;
        }
    }
    return result;
}
#if 0
ns_baseunit -size 1KB
#endif

/*
 *----------------------------------------------------------------------
 *
 * NsTclStrcollObjCmd --
 *
 *      Compare two strings based on the POSIX strcoll_l() command.
 *
 *      Implements "ns_strcoll".
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
NsTclStrcollObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    Tcl_Obj     *arg1Obj, *arg2Obj;
    char        *localeString = NULL;
    Ns_ObjvSpec opts[] = {
        {"-locale", Ns_ObjvString, &localeString, NULL},
        {"--",      Ns_ObjvBreak,  NULL,          NULL},
        {NULL, NULL, NULL, NULL}
    };

    Ns_ObjvSpec args[] = {
        {"string1",  Ns_ObjvObj, &arg1Obj, NULL},
        {"string2",  Ns_ObjvObj, &arg2Obj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        locale_t    locale = 0;

        if (localeString != NULL) {
#ifdef _WIN32
            locale = _create_locale(LC_COLLATE, localeString);
#else
            locale = newlocale(LC_COLLATE_MASK, localeString, (locale_t)0);
#endif
            if (locale == 0) {
                Ns_TclPrintfResult(interp, "specified locale '%s' is not available", localeString);
                result = TCL_ERROR;
            }
        }

        if (result == TCL_OK) {
            Tcl_DString ds1, ds2, *ds1Ptr = &ds1, *ds2Ptr = &ds2;
            TCL_SIZE_T  length1, length2;
            int         comparisonValue;
            const char *string1, *string2;

            Tcl_DStringInit(ds1Ptr);
            Tcl_DStringInit(ds2Ptr);

            string1 = Tcl_GetStringFromObj(arg1Obj, &length1);
            string2 = Tcl_GetStringFromObj(arg2Obj, &length2);
            (void)Tcl_UtfToExternalDString(NULL, string1, length1, ds1Ptr);
            (void)Tcl_UtfToExternalDString(NULL, string2, length2, ds2Ptr);

            errno = 0;
            comparisonValue = strcoll_l(ds1Ptr->string, ds2Ptr->string,
                                        locale != 0 ? locale : nsconf.locale);

            Ns_Log(Debug, "ns_collate: compare '%s' and '%s' using %s (%p) -> %d (%d)",
                   ds1Ptr->string, ds2Ptr->string,
                   localeString == NULL ? "default locale" : localeString,
                   (void*)locale, comparisonValue, errno);

            Tcl_SetObjResult(interp, Tcl_NewIntObj(comparisonValue));

            Tcl_DStringFree(ds1Ptr);
            Tcl_DStringFree(ds2Ptr);
        }

        if (locale != 0) {
#ifdef _WIN32
            _free_locale(locale);
#else
            freelocale(locale);
#endif
        }
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * IpMatchObjCmd --
 *
 *      Checks whether an IP address (IPv4 or IPV6) is in a given CIDR
 *      (Classless Inter-Domain Routing) range. CIDR supports variable-length
 *      subnet masking and specifies an IPv6 or IPv6 address, a slash ('/')
 *      character, and a decimal number representing the significant bits of
 *      the IP address.
 *
 *      Implements "ns_ip match /cidr/ /ipaddr/".
 *
 *      Example:
 *          ns_ip match 137.208.0.0/16 137.208.116.31
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
IpMatchObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    char        *cidrString, *ipString;
    unsigned int nrBits = 0;
    struct NS_SOCKADDR_STORAGE ip, ip2, mask;
    struct sockaddr
        *ipPtr = (struct sockaddr *)&ip,
        *ipPtr2 = (struct sockaddr *)&ip2,
        *maskPtr = (struct sockaddr *)&mask;

    Ns_ObjvSpec args[] = {
        {"cidr",   Ns_ObjvString, &cidrString, NULL},
        {"ipaddr", Ns_ObjvString, &ipString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (ns_inet_pton(ipPtr, ipString) != 1) {
        Ns_TclPrintfResult(interp, "'%s' is not a valid IPv4 or IPv6 address", ipString);
        result = TCL_ERROR;

    } else if (Ns_SockaddrParseIPMask(interp, cidrString, ipPtr2, maskPtr, &nrBits) != NS_OK) {
        Ns_TclPrintfResult(interp, "'%s' is not a valid CIDR string for IPv4 or IPv6", cidrString);
        result = TCL_ERROR;

    } else {
        bool success = (nrBits == 0 || Ns_SockaddrMaskedMatch(ipPtr, maskPtr, ipPtr2));

        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(success));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * IpPropertiesObjCmd --
 *
 *      Implements "ns_ip properties". In case a valid IP address is provided
 *      it returns a dict containing members "trusted", "public" and "type".
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
IpPropertiesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    struct NS_SOCKADDR_STORAGE ip;
    struct sockaddr *ipPtr = (struct sockaddr *)&ip;
    int         result = TCL_OK;
    char       *ipString;
    Ns_ObjvSpec args[] = {
        {"ipaddr", Ns_ObjvString, &ipString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (ns_inet_pton(ipPtr, ipString) != 1) {
        Ns_TclPrintfResult(interp, "'%s' is not a valid IPv4 or IPv6 address", ipString);
        result = TCL_ERROR;

    } else {
        Tcl_Obj *dictObj = Tcl_NewDictObj();

        (void)Ns_SockaddrAddToDictIpProperties(ipPtr, dictObj);
        Tcl_SetObjResult(interp, dictObj);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * IpPublicObjCmd --
 *
 *      Implements "ns_ip public", returning a boolean value in case the
 *      provided IP address is valid.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
IpPublicObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    struct NS_SOCKADDR_STORAGE ip;
    struct sockaddr *ipPtr = (struct sockaddr *)&ip;
    char       *ipString;
    Ns_ObjvSpec args[] = {
        {"ipaddr", Ns_ObjvString, &ipString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (ns_inet_pton(ipPtr, ipString) != 1) {
        Ns_TclPrintfResult(interp, "'%s' is not a valid IPv4 or IPv6 address", ipString);
        result = TCL_ERROR;

    } else {
        bool isPublic = Ns_SockaddrPublicIpAddress(ipPtr);

        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(isPublic));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * IpTrustedObjCmd --
 *
 *      Implements "ns_ip trusted", returning a boolean value in case the
 *      provided IP address is valid.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
IpTrustedObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    struct NS_SOCKADDR_STORAGE ip;
    struct sockaddr *ipPtr = (struct sockaddr *)&ip;
    char       *ipString;
    Ns_ObjvSpec args[] = {
        {"ipaddr", Ns_ObjvString, &ipString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (ns_inet_pton(ipPtr, ipString) != 1) {
        Ns_TclPrintfResult(interp, "'%s' is not a valid IPv4 or IPv6 address", ipString);
        result = TCL_ERROR;

    } else {
        bool isTrusted = Ns_SockaddrTrustedReverseProxy(ipPtr);

        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(isTrusted));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * IpValidObjCmd --
 *
 *      Implements "ns_ip valid", returning a boolean value indicating if the
 *      provided IP address is valid. One can provide "-type ipv4" or "-type
 *      ipv6" to constrain the result to these address families.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ObjvTable addressTypeSet[] = {
    // {"IPv4",    4u},  /* we have in other enumeration no case-insensitivity */
    // {"IPv6",    6u},
    {"ipv4",    4u},
    {"ipv6",    6u},
    {NULL,      0u}
};

static int
IpValidObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    struct NS_SOCKADDR_STORAGE ip;
    struct sockaddr *ipPtr = (struct sockaddr *)&ip;
    int         result = TCL_OK, addressType = 0;
    char       *ipString;
    Ns_ObjvSpec lopts[] = {
        {"-type", Ns_ObjvIndex, &addressType, addressTypeSet},
        {"--",    Ns_ObjvBreak, NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"ipaddr", Ns_ObjvString, &ipString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        bool valid = (ns_inet_pton(ipPtr, ipString) == 1);
        if (valid && addressType != 0) {
            valid = (   ((addressType == 4) && (ipPtr->sa_family == AF_INET))
                     || ((addressType == 5) && (ipPtr->sa_family == AF_INET6))
                     );
        }
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(valid));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclIpObjCmd --
 *
 *      Implements "ns_ip".
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
NsTclIpObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"match",      IpMatchObjCmd},
        {"properties", IpPropertiesObjCmd},
        {"public",     IpPublicObjCmd},
        {"trusted",    IpTrustedObjCmd},
        {"valid",      IpValidObjCmd},
        /*ns_ip lookup ?-all? /ipaddr/        {NULL, NULL}*/
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}



/*
 *----------------------------------------------------------------------
 *
 * GetCsvObjCmd --
 *
 *      Implements "ns_getcsv". The command reads a single line from a
 *      CSV file and parses the results into a Tcl list variable.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      One line is read for given open channel.
 *
 *----------------------------------------------------------------------
 */
static void
FinishElement(Tcl_DString *elemPtr, Tcl_DString *colsPtr, bool quoted)
{
    if (!quoted) {
        Tcl_DStringAppendElement(colsPtr, Ns_StrTrim(elemPtr->string));
    } else {
        Tcl_DStringAppendElement(colsPtr, elemPtr->string);
    }
    Tcl_DStringSetLength(elemPtr, 0);
}


int
NsTclGetCsvObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int           trimUnquoted = 0, result = TCL_OK;
    char         *delimiter = (char *)",", *quoteString = (char *)"\"", *fileId, *varName;
    Tcl_Channel   chan;
    Ns_ObjvSpec   opts[] = {
        {"-delimiter", Ns_ObjvString,   &delimiter,    NULL},
        {"-quotechar", Ns_ObjvString,   &quoteString,  NULL},
        {"-trim",      Ns_ObjvBool,     &trimUnquoted, INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak,    NULL,          NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   args[] = {
        {"channelId",     Ns_ObjvString, &fileId,   NULL},
        {"varname",    Ns_ObjvString, &varName,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpenChannel(interp, fileId, 0, NS_FALSE, &chan) == TCL_ERROR) {
        result = TCL_ERROR;

    } else {
        int             ncols;
        bool            inquote, quoted, emptyElement;
        const char     *p, *value;
        Tcl_DString     line, cols, elem;
        char            c, quote = *quoteString;

        Tcl_DStringInit(&line);
        Tcl_DStringInit(&cols);
        Tcl_DStringInit(&elem);

        ncols = 0;
        inquote = NS_FALSE;
        quoted = NS_FALSE;
        emptyElement = NS_TRUE;

        for (;;) {
            if (Tcl_Gets(chan, &line) == TCL_IO_FAILURE) {
                Tcl_DStringFree(&line);
                Tcl_DStringFree(&cols);
                Tcl_DStringFree(&elem);

                if (Tcl_Eof(chan) == 0) {
                    Ns_TclPrintfResult(interp, "could not read from %s: %s",
                                       fileId, Tcl_PosixError(interp));
                    return TCL_ERROR;
                }
                Tcl_SetObjResult(interp, Tcl_NewIntObj(-1));
                return TCL_OK;
            }

            p = line.string;
            while (*p != '\0') {
                c = *p++;
            loopstart:
                if (inquote) {
                    if (c == quote) {
                        c = *p++;
                        if (c == '\0') {
                            /*
                             * Line ends after quote
                             */
                            inquote = NS_FALSE;
                            break;
                        }
                        if (c == quote) {
                            /*
                             * We have a quote in the quote.
                             */
                            Tcl_DStringAppend(&elem, &c, 1);
                        } else {
                            inquote = NS_FALSE;
                            goto loopstart;
                        }
                    } else {
                        Tcl_DStringAppend(&elem, &c, 1);
                    }
                } else {
                    if (c == quote && emptyElement) {
                        inquote = NS_TRUE;
                        quoted = NS_TRUE;
                        emptyElement = NS_FALSE;
                    } else if (strchr(delimiter, INTCHAR(c)) != NULL) {
                        FinishElement(&elem, &cols, (trimUnquoted ? quoted : 1));
                        ncols++;
                        quoted = NS_FALSE;
                        emptyElement = NS_TRUE;
                    } else {
                        emptyElement = NS_FALSE;
                        if (!quoted) {
                            Tcl_DStringAppend(&elem, &c, 1);
                        }
                    }
                }
            }
            if (inquote) {
                Tcl_DStringAppend(&elem, "\n", 1);
                Tcl_DStringSetLength(&line, 0);
                continue;
            }
            break;
        }

        if (!(ncols == 0 && emptyElement)) {
            FinishElement(&elem, &cols, (trimUnquoted ? quoted : 1));
            ncols++;
        }
        value = Tcl_SetVar(interp, varName, cols.string, TCL_LEAVE_ERR_MSG);
        Tcl_DStringFree(&line);
        Tcl_DStringFree(&cols);
        Tcl_DStringFree(&elem);

        if (value == NULL) {
            result = TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(ncols));
        }
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
