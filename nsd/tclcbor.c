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
 * tclcbor.c --
 *
 *      Implements a lot of Tcl API commands.
 *
 * Minimal CBOR decoder for WebAuthn needs.
 *
 * Supported:
 *   - uint / nint (major 0/1) up to 64-bit signed range
 *   - bstr (major 2) definite length
 *   - tstr (major 3) definite length (assumed UTF-8, returned as Tcl string)
 *   - array (major 4) definite length
 *   - map (major 5) definite length
 *   - simple values: false/true/null/undefined (major 7, ai 20..23)
 *
 * Unsupported (error):
 *   - tags (major 6)
 *   - indefinite-length items (ai=31)
 *   - floats/simple values outside 20..23
 *   - integers beyond signed 64-bit range
 *
 * Parameters:
 *   interp    Tcl interpreter for errors
 *   pPtr      in/out pointer to current position in buffer
 *   end       end of buffer
 *   depth     recursion depth guard
 *   encoding  how to represent CBOR byte strings in Tcl
 *   scratch   scratch buffer for EncodedObj() (hex/base64)
 *   scratchSz size of scratch
 *   objPtr    output Tcl object
 *
 */

#include "nsd.h"

#ifndef CBOR_MAX_DEPTH
# define CBOR_MAX_DEPTH 64
#endif

/*
 * Local functions defined in this file
 */

static TCL_OBJCMDPROC_T   CborDecodeObjCmd;

static int
CborNeed(Tcl_Interp *interp, const uint8_t *p, const uint8_t *end, size_t n)
    NS_GNUC_NONNULL(1,2,3);

static int
CborReadU8(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint8_t *vPtr)
    NS_GNUC_NONNULL(1,2,3,4);

static int
CborReadBE16(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint16_t *vPtr)
    NS_GNUC_NONNULL(1,2,3,4);

static int
CborReadBE32(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint32_t *vPtr)
    NS_GNUC_NONNULL(1,2,3,4);

static int
CborReadBE64(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint64_t *vPtr)
    NS_GNUC_NONNULL(1,2,3,4);

static int
CborReadArg(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end,
            uint8_t ai, uint64_t *argPtr)
    NS_GNUC_NONNULL(1,2,3,5);

static Tcl_Obj *
CborMakeBstrObj(const uint8_t *bytes, uint64_t len,
                Ns_BinaryEncoding encoding, Tcl_DString *scratchDsPtr)
        NS_GNUC_NONNULL(1,4);

static int
CborDecodeAny(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end,
              int depth, Ns_BinaryEncoding encoding, Tcl_DString *scratchDsPtr,
              Tcl_Obj **objPtr)
    NS_GNUC_NONNULL(1,2,3,6,7);



/*
 *----------------------------------------------------------------------
 *
 * CborNeed --
 *
 *      Verify that at least 'n' bytes remain between the current read pointer
 *      'p' and the end pointer 'end' when parsing CBOR input.
 *
 * Parameters:
 *      interp - Tcl interpreter for error reporting; must not be NULL.
 *      p      - Current read pointer into the input buffer (inclusive).
 *      end    - End pointer (one past the last valid byte) of the input buffer.
 *      n      - Number of bytes required to be available from 'p'.
 *
 * Results:
 *      TCL_OK if (end - p) >= n; otherwise TCL_ERROR after setting the
 *      interpreter result to "CBOR truncated input".
 *
 * Side effects:
 *      On error, writes a diagnostic message into 'interp'. No other
 *      side effects.
 *
 *----------------------------------------------------------------------
 */

static int
CborNeed(Tcl_Interp *interp, const uint8_t *p, const uint8_t *end, size_t n)
{
    int result = TCL_OK;

    if ((size_t)(end - p) < n) {
        Ns_TclPrintfResult(interp, "CBOR truncated input");
        result = TCL_ERROR;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CborReadU8 / CborReadBE16 / CborReadBE32 / CborReadBE64 --
 *
 *      Read an unsigned integer of width 8/16/32/64 bits in big-endian order
 *      from a CBOR input buffer, store it in *vPtr, and advance the caller's
 *      read cursor *pPtr by the number of bytes read.
 *
 * Parameters:
 *      interp - Tcl interpreter for error reporting; must not be NULL.
 *      pPtr   - Address of the current read pointer into the input buffer;
 *               on success, advanced by 1/2/4/8 bytes respectively.
 *      end    - End pointer (one past the last valid byte) of the input buffer.
 *      vPtr   - Output pointer receiving the decoded value (uint8_t/uint16_t/
 *               uint32_t/uint64_t as appropriate); must not be NULL.
 *
 * Results:
 *      TCL_OK on success. TCL_ERROR if fewer than the required bytes remain
 *      between *pPtr and end; in this case the interpreter result is set to
 *      "CBOR truncated input" by CborNeed().
 *
 * Side effects:
 *      On success, *pPtr is advanced and *vPtr is written. On error, *pPtr is
 *      not advanced and *vPtr is not modified. No global state is changed.
 *
 *----------------------------------------------------------------------
 */
static int
CborReadU8(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint8_t *vPtr)
{
    int result = TCL_OK;
    const uint8_t *p = *pPtr;

    if (CborNeed(interp, p, end, 1) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        *vPtr = *p++;
        *pPtr = p;
    }
    return result;
}

static int
CborReadBE16(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint16_t *vPtr)
{
    int result = TCL_OK;
    const uint8_t *p = *pPtr;

    if (CborNeed(interp, p, end, 2) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
        *vPtr = v;
        *pPtr = p + 2;
    }
    return result;
}

static int
CborReadBE32(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint32_t *vPtr)
{
    int result = TCL_OK;
    const uint8_t *p = *pPtr;
    if (CborNeed(interp, p, end, 4) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
            | ((uint32_t)p[2] <<  8) | ((uint32_t)p[3]);
        *vPtr = v;
        *pPtr = p + 4;
    }
    return result;
}

static int
CborReadBE64(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end, uint64_t *vPtr)
{
    int result = TCL_OK;
    const uint8_t *p = *pPtr;

    if (CborNeed(interp, p, end, 8) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        uint64_t v = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48)
            | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
            | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16)
            | ((uint64_t)p[6] <<  8) | ((uint64_t)p[7]);
        *vPtr = v;
        *pPtr = p + 8;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CborReadArg --
 *
 *      Parse the CBOR "additional information" (ai) field of an initial byte
 *      and return the associated numeric argument in *argPtr.  Supports ai
 *      values 0..27 (direct/immediate and 1/2/4/8‑byte integers); rejects ai
 *      == 31 (indefinite length) in this minimal decoder.
 *
 * Parameters:
 *      interp - Tcl interpreter for error reporting; must not be NULL.
 *      pPtr   - Address of the current read cursor into the input buffer;
 *               advanced by 0/1/2/4/8 bytes depending on ai.
 *      end    - End pointer (one past the last valid byte) of the input buffer.
 *      ai     - The 5-bit additional-information value from the initial byte.
 *      argPtr - Output pointer receiving the decoded argument (0..2^64-1);
 *               must not be NULL.
 *
 * Results:
 *      TCL_OK on success. TCL_ERROR if ai is unsupported/invalid (e.g., 31 for
 *      indefinite-length) or if insufficient input remains; on error, the
 *      interpreter result is set (e.g., "CBOR truncated input" or a specific
 *      diagnostic via Ns_TclPrintfResult()).
 *
 * Side effects:
 *      On success, *pPtr is advanced and *argPtr is written. On error, *pPtr is
 *      not advanced and *argPtr is not modified.
 *
 * Notes:
 *      - ai < 24 encodes the value directly in ai.
 *      - ai == 24 -> read uint8; ai == 25 -> read big-endian uint16;
 *        ai == 26 -> read big-endian uint32; ai == 27 -> read big-endian uint64.
 *      - Bounds checking is delegated to CborReadU8/BE16/BE32/BE64, which call
 *        CborNeed() to validate available input.
 *      - Indefinite-length (ai == 31) is not supported and results in error.
 *
 *----------------------------------------------------------------------
 */
static int
CborReadArg(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end,
            uint8_t ai, uint64_t *argPtr)
{
    int result = TCL_OK;

    if (ai < 24) {
        *argPtr = (uint64_t)ai;
    } else if (ai == 24) {
        uint8_t v;

        if (CborReadU8(interp, pPtr, end, &v) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            *argPtr = (uint64_t)v;
        }
    } else if (ai == 25) {
        uint16_t v;

        if (CborReadBE16(interp, pPtr, end, &v) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            *argPtr = (uint64_t)v;
        }
    } else if (ai == 26) {
        uint32_t v;

        if (CborReadBE32(interp, pPtr, end, &v) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            *argPtr = (uint64_t)v;
        }
    } else if (ai == 27) {
        uint64_t v;

        if (CborReadBE64(interp, pPtr, end, &v) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            *argPtr = v;
        }
    } else {
        /* ai == 31 -> indefinite length; others are reserved/invalid here */
        Ns_TclPrintfResult(interp, (ai == 31)
                           ? "CBOR indefinite-length items not supported"
                           : "CBOR invalid additional-info");
        result = TCL_ERROR;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CborMakeBstrObj --
 *
 *      Create a Tcl object for a CBOR byte string either as a bytearray
 *      (binary) or as a text-encoded representation, depending on the
 *      requested Ns_BinaryEncoding.
 *
 * Parameters:
 *      bytes        - Pointer to input bytes (must not be NULL when len > 0).
 *      len          - Length of the input in bytes (0..2^64-1). For the binary
 *                     path, the length must fit into an int (as required by
 *                     Tcl_NewByteArrayObj).
 *      encoding     - Target encoding selector. When NS_OBJ_ENCODING_BINARY,
 *                     a Tcl bytearray is produced; otherwise a text object is
 *                     created using NsEncodedObj().
 *      scratchDsPtr - Scratch Tcl_DString used as temporary output buffer for
 *                     NsEncodedObj() when a text encoding is requested; its
 *                     length is adjusted as needed.
 *
 * Results:
 *      A newly created Tcl_Obj* holding the CBOR byte string in the requested
 *      form. Reference count is 0 on return. Never returns NULL on success.
 *
 * Side effects:
 *      When a text encoding is requested, scratchDsPtr is resized and its
 *      internal buffer is written with the encoded data before constructing
 *      the Tcl object. No other global state is modified.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
CborMakeBstrObj(const uint8_t *bytes, uint64_t len,
                Ns_BinaryEncoding encoding, Tcl_DString *scratchDsPtr)
{
    Tcl_Obj *resultObj;

    if (encoding == NS_OBJ_ENCODING_BINARY) {
        resultObj = Tcl_NewByteArrayObj((const unsigned char *)bytes, (int)len);
    } else {
        TCL_SIZE_T need = NsEncodedObjScratchSize(encoding, len);

        Tcl_DStringSetLength(scratchDsPtr, need);
        resultObj = NsEncodedObj((unsigned char *)bytes, len, scratchDsPtr->string, encoding);
    }
    return resultObj;
}

/*
 *----------------------------------------------------------------------
 *
 * CborDecodeAny --
 *
 *      Decode a single CBOR item from the input range [*pPtr, end) into a Tcl
 *      object and advance *pPtr past the consumed bytes. Supports major types
 *      0 (unsigned), 1 (negative), 2 (byte string), 3 (text string), 4
 *      (array; fixed length), 5 (map; fixed length), and selected simple
 *      values in major type 7 (false, true, null, undefined). Tags and
 *      floating‑point/simple values other than the listed booleans/null/
 *      undefined are rejected. Indefinite lengths are not supported.
 *
 * Parameters:
 *      interp       - Tcl interpreter for error reporting; must not be NULL.
 *      pPtr         - Address of the current read cursor; advanced on success.
 *      end          - One‑past‑the‑end pointer of the input buffer.
 *      depth        - Current nesting depth; used to enforce CBOR_MAX_DEPTH.
 *      encoding     - Binary/text encoding used for CBOR byte strings (major 2).
 *      scratchDsPtr - Scratch DString used when producing encoded text for
 *                     byte strings; must be valid when encoding !=
 *                     NS_OBJ_ENCODING_BINARY.
 *      objPtr       - Out parameter receiving a newly created Tcl_Obj* with the
 *                     decoded value (refcount 0 on success).
 *
 * Results:
 *      TCL_OK on success (and *objPtr set; *pPtr advanced). TCL_ERROR on parse
 *      error, unsupported feature (tags, floats, indefinite lengths), truncation,
 *      or excessive nesting; in these cases, the interpreter result is set to a
 *      diagnostic message and *pPtr is not advanced.
 *
 * Side effects:
 *      May allocate Tcl objects for the decoded value, list (array), or dict
 *      (map). Uses helper readers (CborReadU8/BE16/BE32/BE64, CborReadArg,
 *      CborNeed) for bounds checking and numeric extraction. No global state is
 *      modified.
 *
 *----------------------------------------------------------------------
 */
static int
CborDecodeAny(Tcl_Interp *interp, const uint8_t **pPtr, const uint8_t *end,
              int depth, Ns_BinaryEncoding encoding, Tcl_DString *scratchDsPtr,
              Tcl_Obj **objPtr)
{
    const uint8_t *p = *pPtr;
    uint8_t        ib, major, ai;
    uint64_t       arg;

    if (depth > CBOR_MAX_DEPTH) {
        Ns_TclPrintfResult(interp, "CBOR nesting too deep");
        return TCL_ERROR;
    }

    if (CborReadU8(interp, &p, end, &ib) != TCL_OK) {
        return TCL_ERROR;
    }

    major = (uint8_t)(ib >> 5);
    ai    = (uint8_t)(ib & 0x1Fu);
    arg  = 0;

    switch (major) {
    case 0: /* unsigned int */
        if (CborReadArg(interp, &p, end, ai, &arg) != TCL_OK) {
            return TCL_ERROR;
        } else if (arg > (uint64_t)WIDE_INT_MAX) {
            Ns_TclPrintfResult(interp, "CBOR integer too large for Tcl wide int");
            return TCL_ERROR;
        }
        *objPtr = Tcl_NewWideIntObj((Tcl_WideInt)arg);
        break;

    case 1: /* negative int: value = -1 - arg */
        if (CborReadArg(interp, &p, end, ai, &arg) != TCL_OK) {
            return TCL_ERROR;
        } else if (arg > (uint64_t)WIDE_INT_MAX) {
            Ns_TclPrintfResult(interp, "CBOR negative integer too small for Tcl wide int");
            return TCL_ERROR;
        }
        *objPtr = Tcl_NewWideIntObj((Tcl_WideInt)(-1 - (Tcl_WideInt)arg));
        break;

    case 2: /* byte string */
        if (CborReadArg(interp, &p, end, ai, &arg) != TCL_OK) {
            return TCL_ERROR;
        } else if (CborNeed(interp, p, end, (size_t)arg) != TCL_OK) {
            return TCL_ERROR;
        }
        *objPtr = CborMakeBstrObj(p, arg, encoding, scratchDsPtr);
        p += (size_t)arg;
        break;

    case 3: /* text string */
        if (CborReadArg(interp, &p, end, ai, &arg) != TCL_OK) {
            return TCL_ERROR;
        } else if (CborNeed(interp, p, end, (size_t)arg) != TCL_OK) {
            return TCL_ERROR;
        }
        *objPtr = Tcl_NewStringObj((const char *)p, (int)arg);
        p += (size_t)arg;
        break;

    case 4: { /* array */
        if (CborReadArg(interp, &p, end, ai, &arg) != TCL_OK) {
            return TCL_ERROR;
        } else {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);
            for (uint64_t i = 0; i < arg; i++) {
                Tcl_Obj *elem = NULL;
                if (CborDecodeAny(interp, &p, end, depth + 1, encoding, scratchDsPtr, &elem) != TCL_OK) {
                    return TCL_ERROR;
                }
                Tcl_ListObjAppendElement(interp, listObj, elem);
            }
            *objPtr = listObj;
        }
        break;
    }

    case 5: { /* map */
        if (CborReadArg(interp, &p, end, ai, &arg) != TCL_OK) {
            return TCL_ERROR;
        } else {
            Tcl_Obj *dictObj = Tcl_NewDictObj();
            for (uint64_t i = 0; i < arg; i++) {
                Tcl_Obj *k = NULL, *v = NULL;
                if (CborDecodeAny(interp, &p, end, depth + 1, encoding, scratchDsPtr, &k) != TCL_OK) {
                    return TCL_ERROR;
                }
                if (CborDecodeAny(interp, &p, end, depth + 1, encoding, scratchDsPtr, &v) != TCL_OK) {
                    return TCL_ERROR;
                }
                Tcl_DictObjPut(interp, dictObj, k, v);
            }
            *objPtr = dictObj;
        }
        break;
    }

    case 6: /* tag */
        Ns_TclPrintfResult(interp, "CBOR tags not supported");
        return TCL_ERROR;

    case 7: /* simple / floats */
        if (ai == 20) { /* false */
            *objPtr = Tcl_NewBooleanObj(0);
        } else if (ai == 21) { /* true */
            *objPtr = Tcl_NewBooleanObj(1);
        } else if (ai == 22) { /* null */
            *objPtr = Tcl_NewObj();  /* empty */
        } else if (ai == 23) { /* undefined */
            *objPtr = Tcl_NewObj();  /* empty */
        } else {
            Ns_TclPrintfResult(interp, "CBOR simple/float value not supported");
            return TCL_ERROR;
        }
        break;

    default:
        Ns_TclPrintfResult(interp, "CBOR invalid major type");
        return TCL_ERROR;
    }

    *pPtr = p;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CborDecodeObjCmd --
 *
 *      Implements both "ns_cbor decode" and "ns_cbor scan". Parses options,
 *      decodes a single CBOR item from the input object, and returns either
 *      the decoded value (decode) or a two‑element list {value
 *      bytes_consumed} (scan). Byte strings (major type 2) are produced as a
 *      bytearray or as a text‑encoded object depending on the selected
 *      Ns_BinaryEncoding.
 *
 * Parameters:
 *      clientData - Unused.
 *      interp     - Tcl interpreter for results and error reporting; must not be NULL.
 *      objc       - Argument count.
 *      objv       - Argument vector: subcommand followed by options and the CBOR object.
 *                   Recognized options:
 *                     -binary           boolean; when true, interpret the input as a
 *                                       Tcl bytearray / binary data source.
 *                     -encoding         one of the values in "binaryencodings"; selects
 *                                       how CBOR byte strings are represented (defaults
 *                                       to NS_OBJ_ENCODING_BINARY when omitted).
 *                   Positional arguments:
 *                     cbor              the CBOR data to decode (string or bytearray).
 *
 * Results:
 *      TCL_OK on success, with the interpreter result set to either the decoded
 *      value ("decode") or a list {value bytes_consumed} ("scan"). TCL_ERROR on
 *      option/argument errors or CBOR decoding errors; in these cases a diagnostic
 *      message is placed in the interpreter result.
 *
 * Side effects:
 *      - Obtains a binary pointer/length view of the input via Ns_GetBinaryString().
 *      - Calls CborDecodeAny() to perform the actual parsing; advances an internal
 *        cursor over the consumed bytes and, for "scan", reports the number of bytes
 *        consumed relative to the start of the buffer.
 *      - Uses a scratch Tcl_DString as temporary storage when producing text‑encoded
 *        representations of CBOR byte strings.
 *
 *----------------------------------------------------------------------
 */

static int
CborDecodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                 TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result, isBinary = 0, encodingInt = -1;
    Tcl_Obj *cborObj;

    Ns_ObjvSpec lopts[] = {
        {"-binary",   Ns_ObjvBool,  &isBinary,    INT2PTR(NS_TRUE)},
        {"-encoding", Ns_ObjvIndex, &encodingInt, binaryencodings},
        {"--",        Ns_ObjvBreak, NULL,         NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"value", Ns_ObjvObj, &cborObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_BinaryEncoding    encoding = (encodingInt == -1
                                         ? NS_OBJ_ENCODING_BINARY
                                         : (Ns_BinaryEncoding)encodingInt);
        TCL_SIZE_T           cborLength;
        const unsigned char *cborString;
        Tcl_DString          cborDs, scratchDs;
        Tcl_Obj             *resultObj = NULL;
        const uint8_t       *p, *start, *end;

        Tcl_DStringInit(&cborDs);
        Tcl_DStringInit(&scratchDs);
        cborString = Ns_GetBinaryString(cborObj, isBinary == 1, &cborLength, &cborDs);
        start = cborString;
        p = start;
        end = start + cborLength;

        result = CborDecodeAny(interp, &p, end, 0, encoding, &scratchDs, &resultObj);
        if (result == TCL_OK) {
            const char  *subcmdName = Tcl_GetString(objv[1]);

            if (*subcmdName == 'd') {
                Tcl_SetObjResult(interp, resultObj);
            } else /* if (*subcmdName == 's') */ {
                Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

                Tcl_ListObjAppendElement(interp, listObj, resultObj);
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj((Tcl_WideInt)(p - start)));
                Tcl_SetObjResult(interp, listObj);
            }
        }
        Tcl_DStringFree(&scratchDs);
        Tcl_DStringFree(&cborDs);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCborObjCmd --
 *
 *      This command implements the "ns_cbor" command, dispatches the
 *      subcommands "decode" and "scan" to CborDecodeObjCmd via
 *      Ns_SubcmdObjv().
 *
 * Results:
 *      A standard Tcl result. TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      Depends on the specific subcommand invoked.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCborObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"decode", CborDecodeObjCmd},
        {"scan",   CborDecodeObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
