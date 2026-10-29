/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * scan.c --
 *
 *      Routines for parsing the options and arguments passed to Tcl
 *      commands.  Bounded, snapshot-based access-log scanning.  This
 *      producer owns compact lexical storage adopted by the optional
 *      nsdb batch interface; no driver, SQL, numeric coercion or
 *      application redaction is involved.  The batch layout is
 *      private, not a published ABI.
 */

#include "../nsd/nsd.h"
#include "scan.h"
#include "../nsd/nsatoms.h"
#include "../nsdb/batch.h"

#define SCAN_BUFFER 65536u
#define SCAN_PREFIX 64u
#define SCAN_EXCERPT 256u
#define SCAN_TOKENS 256u

enum { T_STRING, T_NUMBER, T_BOOLEAN, T_NULL, T_OBJECT, T_ARRAY, N_TYPES };

enum {
    F_OFFSET,
    F_END,
    F_TIMESTAMP,
    F_PEER,
    F_THREAD,
    F_REQUESTID,
    F_USER,
    F_REQUEST,
    F_METHOD,
    F_TARGET,
    F_PATH,
    F_PROTOCOL,
    F_STATUS,
    F_BYTES,
    F_REFERER,
    F_AGENT,
    F_REQTIME,
    F_START,
    F_ACCEPT,
    F_QUEUE,
    F_FILTER,
    F_RUN,
    F_SOURCE,
    F_DRIVER,
    F_POOL,
    F_PHASE,
    F_REASON,
    F_EXTRA,
    F_PARSE,
    F_DIAGNOSTIC,
    F_EXCERPT,
    F_TRUNCATED,
    F_LINES,
    N_FIELDS
};

typedef struct {
    NsAtomId      name, refinement, representation;
    unsigned char type;
} Field;

static const Field schema[N_FIELDS] = {
    {NS_ATOM_offset, NS_ATOM_int64, NS_ATOM_integer_decimal, T_NUMBER},
    {NS_ATOM_endOffset, NS_ATOM_int64, NS_ATOM_integer_decimal, T_NUMBER},
    {NS_ATOM_timestamp, NS_ATOM_accesslog_time, NS_ATOM_nslog_timestamp, T_STRING},
    {NS_ATOM_peerAddress, -1, -1, T_STRING},
    {NS_ATOM_threadName, -1, -1, T_STRING},
    {NS_ATOM_requestIdentifier, -1, -1, T_STRING},
    {NS_ATOM_user, -1, -1, T_STRING},
    {NS_ATOM_requestLine, -1, -1, T_STRING},
    {NS_ATOM_method, -1, -1, T_STRING},
    {NS_ATOM_target, -1, -1, T_STRING},
    {NS_ATOM_path, -1, -1, T_STRING},
    {NS_ATOM_protocol, -1, -1, T_STRING},
    {NS_ATOM_status, NS_ATOM_int32, NS_ATOM_integer_decimal, T_NUMBER},
    {NS_ATOM_responseBytes, NS_ATOM_int64, NS_ATOM_integer_decimal, T_NUMBER},
    {NS_ATOM_referer, -1, -1, T_STRING},
    {NS_ATOM_userAgent, -1, -1, T_STRING},
    {NS_ATOM_requestTime, NS_ATOM_duration_us, NS_ATOM_seconds_decimal, T_NUMBER},
    {NS_ATOM_startTime, NS_ATOM_datetime_us, NS_ATOM_unix_seconds_decimal, T_NUMBER},
    {NS_ATOM_acceptTime, NS_ATOM_duration_us, NS_ATOM_seconds_decimal, T_NUMBER},
    {NS_ATOM_queueTime, NS_ATOM_duration_us, NS_ATOM_seconds_decimal, T_NUMBER},
    {NS_ATOM_filterTime, NS_ATOM_duration_us, NS_ATOM_seconds_decimal, T_NUMBER},
    {NS_ATOM_runTime, NS_ATOM_duration_us, NS_ATOM_seconds_decimal, T_NUMBER},
    {NS_ATOM_recordSource, -1, -1, T_STRING},
    {NS_ATOM_driver, -1, -1, T_STRING},
    {NS_ATOM_pool, -1, -1, T_STRING},
    {NS_ATOM_phaseTimingAvailable, -1, -1, T_BOOLEAN},
    {NS_ATOM_phaseTimingReason, -1, -1, T_STRING},
    {NS_ATOM_extendedFields, -1, NS_ATOM_nslog_fields, T_STRING},
    {NS_ATOM_parseStatus, -1, -1, T_STRING},
    {NS_ATOM_diagnostic, -1, -1, T_STRING},
    {NS_ATOM_excerpt, NS_ATOM_encoded_binary, NS_ATOM_hex, T_STRING},
    {NS_ATOM_truncated, -1, -1, T_BOOLEAN},
    {NS_ATOM_physicalLineCount, NS_ATOM_int32, NS_ATOM_integer_decimal, T_NUMBER}};

typedef struct {
    uint32_t offset, length;
} Cell;

typedef struct {
    uint64_t present;

    Cell cells[N_FIELDS];
} Row;

typedef struct {
    const char *bytes;
    size_t      length;
    bool        present;
} Slice;

typedef struct {
    size_t offset, length, rawStart;
    char   kind;
} Token;
typedef struct Scan Scan;

typedef struct {
    unsigned             refs;
    const Ns_DbBatchApi *batchApi;
    Ns_DbBatchSchema    *batchSchema;
    Tcl_Interp          *interp;
    Scan                *scans;
    Tcl_Obj     *names[N_FIELDS], *types[N_TYPES], *nullValue, *trueValue, *falseValue, *schema;
    Tcl_Encoding utf8;
} ScanState;

struct Scan {
    unsigned      refs;
    ScanState    *owner;
    Scan         *prev, *next;
    int           fd, format, timing, requestTime;
    TCL_SIZE_T    headerCount;
    Tcl_Obj      *extendedHeaders;
    bool          final, oversized;
    char         *filename;
    Tcl_WideInt   device, inode, snapshotEnd, offset, safeOffset, lineStart;
    size_t        maxLine, lineLength, bufferPos, bufferLength, prefixLength;
    unsigned char prefix[SCAN_PREFIX], buffer[SCAN_BUFFER];
    char         *line;
};

typedef struct {
    unsigned    refs;
    ScanState  *owner;
    Row        *rows;
    size_t      count, capacity;
    Tcl_DString arena;
    Tcl_Obj    *metadata;
} Batch;

static Tcl_FreeInternalRepProc ScannerFree;
static Tcl_DupInternalRepProc  ScannerDup;
static Tcl_UpdateStringProc    OpaqueString;
static Tcl_ObjType scannerType = {"nslog:scanner", ScannerFree, ScannerDup, OpaqueString,
                                  Ns_TclSetFromAnyError};

static TCL_OBJCMDPROC_T OpenScanObjCmnd;
static TCL_OBJCMDPROC_T ScanNextObjCmd;
static TCL_OBJCMDPROC_T ScanInfoObjCmd;
static TCL_OBJCMDPROC_T ScanCloseObjCmd;
static TCL_OBJCMDPROC_T ScanTriplesObjCmd;

static Scan    *GetScan(Tcl_Interp *interp, Tcl_Obj *obj) NS_GNUC_NONNULL(1, 2);
static void     StateRelease(ScanState *state) NS_GNUC_NONNULL(1);
static Tcl_Obj *SchemaInfo(ScanState *state) NS_GNUC_NONNULL(1);
static Tcl_Obj *NormalizeHeaders(Tcl_Interp *interp, Tcl_Obj *headers) NS_GNUC_NONNULL(1,2);

NS_STATIC_ASSERT(N_FIELDS <= 64, "present bitmap must cover all fields");

/*
 *----------------------------------------------------------------------
 *
 * ScanError --
 *
 *      Report a scanner error with its structured Tcl error code.
 *
 * Results:
 *      TCL_ERROR.
 *
 * Side effects:
 *      Replaces the interpreter result and error code.
 *
 *----------------------------------------------------------------------
 */
static int
ScanError(Tcl_Interp *interp, const char *code, const char *message)
{
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message, TCL_INDEX_NONE));
    Tcl_SetErrorCode(interp, "NS_ACCESSLOG", "SCAN", code, NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * DictPut --
 *
 *      Store a value under a core atom key in a dictionary.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates the dictionary and retains the value.
 *
 *----------------------------------------------------------------------
 */
static void
DictPut(Tcl_Obj *dict, NsAtomId key, Tcl_Obj *value)
{
    (void)Tcl_DictObjPut(NULL, dict, NsAtomObj(key), value);
}

/*
 *----------------------------------------------------------------------
 *
 * WidePut --
 *
 *      Store a wide integer under a core atom key.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates an integer value and updates the dictionary.
 *
 *----------------------------------------------------------------------
 */
static void
WidePut(Tcl_Obj *dict, NsAtomId key, Tcl_WideInt value)
{
    DictPut(dict, key, Tcl_NewWideIntObj(value));
}

/*
 *----------------------------------------------------------------------
 *
 * StringPut --
 *
 *      Store a source-supplied string under a core atom key.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates a string value and updates the dictionary.
 *
 *----------------------------------------------------------------------
 */
static void
StringPut(Tcl_Obj *dict, NsAtomId key, const char *value)
{
    DictPut(dict, key, Tcl_NewStringObj(value, TCL_INDEX_NONE));
}

/*
 *----------------------------------------------------------------------
 *
 * ScanClose --
 *
 *      Close a scanner descriptor, if still open.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Closes the source file and marks the descriptor invalid.
 *
 *----------------------------------------------------------------------
 */
static void
ScanClose(Scan *scan)
{
    if (scan->fd != NS_INVALID_FD) {
        (void)ns_close(scan->fd);
        scan->fd = NS_INVALID_FD;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ScanRelease --
 *
 *      Release a scanner and unlink it at the last reference.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May close the file, unlink the scanner and release owner state.
 *
 *----------------------------------------------------------------------
 */
static void
ScanRelease(Scan *scan)
{
    if (--scan->refs == 0u) {
        ScanState *owner = scan->owner;

        ScanClose(scan);
        if (scan->prev != NULL) {
            scan->prev->next = scan->next;
        } else {
            owner->scans = scan->next;
        }
        if (scan->next != NULL) {
            scan->next->prev = scan->prev;
        }
        if (scan->extendedHeaders != NULL) {
            Tcl_DecrRefCount(scan->extendedHeaders);
        }
        ns_free(scan->filename);
        ns_free(scan->line);
        ns_free(scan);
        StateRelease(owner);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * BatchRelease --
 *
 *      Release compact scanner batch storage at the last reference.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May free rows, lexical arena, metadata and retained scanner state.
 *
 *----------------------------------------------------------------------
 */
static void
BatchRelease(Batch *batch)
{
    if (--batch->refs == 0u) {
        ScanState *owner = batch->owner;

        Tcl_DecrRefCount(batch->metadata);
        Tcl_DStringFree(&batch->arena);
        ns_free(batch->rows);
        ns_free(batch);
        StateRelease(owner);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ScannerFree --
 *
 *      Release the scanner retained by a Tcl object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Drops the scanner reference.
 *
 *----------------------------------------------------------------------
 */
static void
ScannerFree(Tcl_Obj *obj)
{
    ScanRelease(obj->internalRep.twoPtrValue.ptr1);
}

/*
 *----------------------------------------------------------------------
 *
 * ScannerDup --
 *
 *      Duplicate an opaque scanner internal representation.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Retains the scanner and initializes the destination representation.
 *
 *----------------------------------------------------------------------
 */
static void
ScannerDup(Tcl_Obj *src, Tcl_Obj *dst)
{
    Scan *scan = src->internalRep.twoPtrValue.ptr1;

    scan->refs++;
    dst->internalRep.twoPtrValue.ptr1 = scan;
    dst->internalRep.twoPtrValue.ptr2 = NULL;
    dst->typePtr = &scannerType;
}

/*
 *----------------------------------------------------------------------
 *
 * OpaqueString --
 *
 *      Generate a descriptive label for an opaque scanner.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets the Tcl object string representation.
 *
 *----------------------------------------------------------------------
 */
static void
OpaqueString(Tcl_Obj *obj)
{
    const char *label = "nslog-scanner";
    size_t      length = strlen(label);

    /*
     * This updateStringProc must fill the existing object's string rep.
     * Tcl_NewStringObj would create a different object; Tcl_SetStringObj
     * would release the scanner's internal rep. Tcl 8.6 has no
     * Tcl_InitStringRep, so its string buffer must be allocated explicitly.
     */
    obj->bytes = Tcl_Alloc((unsigned int)length + 1u);

    memcpy(obj->bytes, label, length + 1u);
    obj->length = (TCL_SIZE_T)length;
}

/*
 *----------------------------------------------------------------------
 *
 * Wrap --
 *
 *      Create an opaque Tcl object for a retained native pointer.
 *
 * Results:
 *      New Tcl object.
 *
 * Side effects:
 *      Stores the supplied pointer and object type without serializing it.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
Wrap(void *pointer, Tcl_ObjType *type)
{
    Tcl_Obj *obj = Tcl_NewObj();

    Tcl_InvalidateStringRep(obj);
    obj->typePtr = type;
    obj->internalRep.twoPtrValue.ptr1 = pointer;
    obj->internalRep.twoPtrValue.ptr2 = NULL;
    return obj;
}

/*
 *----------------------------------------------------------------------
 *
 * StateRelease --
 *
 *      Release interpreter scanner state at the last reference.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May release schema objects, encoding and native state.
 *
 *----------------------------------------------------------------------
 */
static void
StateRelease(ScanState *state)
{
    if (--state->refs == 0u) {
        for (size_t i = 0u; i < N_FIELDS; i++) {
            Tcl_DecrRefCount(state->names[i]);
        }
        for (size_t i = 0u; i < N_TYPES; i++) {
            Tcl_DecrRefCount(state->types[i]);
        }
        Tcl_DecrRefCount(state->nullValue);
        Tcl_DecrRefCount(state->trueValue);
        Tcl_DecrRefCount(state->falseValue);
        if (state->batchSchema != NULL) {
            state->batchApi->schemaRelease(state->batchSchema);
        }
        Tcl_DecrRefCount(state->schema);
        Tcl_FreeEncoding(state->utf8);
        ns_free(state);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * StateDelete --
 *
 *      Close live scanners when their interpreter state is deleted.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Closes source descriptors and releases the interpreter state
 *      reference.
 *
 *----------------------------------------------------------------------
 */
static void
StateDelete(ClientData data, Tcl_Interp *interp)
{
    ScanState *state = data;

    (void)interp; /* Even a C-held scanner wrapper must not keep an interpreter's file open. */
    for (Scan *scan = state->scans; scan != NULL; scan = scan->next) {
        ScanClose(scan);
    }
    state->interp = NULL;
    StateRelease(state);
}

/*
 *----------------------------------------------------------------------
 *
 * GetState --
 *
 *      Obtain or initialize per-interpreter scanner state and shared batch
 *      API.
 *
 * Results:
 *      Borrowed scanner state, or NULL on error.
 *
 * Side effects:
 *      May load ns_json type objects and allocate retained schema state.
 *
 *----------------------------------------------------------------------
 */
static ScanState *
GetState(Tcl_Interp *interp)
{
    ScanState *state = Tcl_GetAssocData(interp, "nslog:scan", NULL);

    if (state == NULL) { /* Reuse ns_json's atoms and typed null. */
        Tcl_Obj             *args[5], **root, **fields;
        TCL_SIZE_T           count;
        int                  result;
        const Ns_DbBatchApi *batchApi;
        Tcl_Obj             *document;
        const int            indexes[N_TYPES] = {1, 4, 7, 10, 13, 16};

        batchApi = Tcl_GetAssocData(interp, NS_DB_BATCH_ASSOC_DATA, NULL);
        if (batchApi == NULL) {
            (void)ScanError(interp, "UNAVAILABLE",
                            "batch scanning requires an already loaded batch-capable nsdb");
            return NULL;
        }
        if (batchApi->abiVersion != NS_DB_BATCH_ABI || batchApi->structSize < sizeof(*batchApi)) {
            (void)ScanError(interp, "UNAVAILABLE", "incompatible nsdb batch interface");
            return NULL;
        }
        args[0] = NsAtomObj(NS_ATOM_ns_json);
        args[1] = NsAtomObj(NS_ATOM_parse);
        args[2] = NsAtomObj(NS_ATOM_DASH_output);
        args[3] = NsAtomObj(NS_ATOM_triples);
        args[4] = Tcl_NewStringObj(
            "{\"s\":\"\",\"n\":0,\"b\":true,\"z\":null,\"o\":{},\"a\":[],\"f\":false}",
            TCL_INDEX_NONE);
        for (size_t i = 0u; i < 5u; i++) {
            Tcl_IncrRefCount(args[i]);
        }
        result = Tcl_EvalObjv(interp, 5, args, TCL_EVAL_GLOBAL);
        for (size_t i = 0u; i < 5u; i++) {
            Tcl_DecrRefCount(args[i]);
        }
        if (result != TCL_OK) {
            return NULL;
        }
        document = Tcl_GetObjResult(interp);
        if (Tcl_ListObjGetElements(interp, document, &count, &root) != TCL_OK || count != 3
            || Tcl_ListObjGetElements(interp, root[2], &count, &fields) != TCL_OK || count != 21) {
            (void)ScanError(interp, "UNAVAILABLE", "ns_json triples interface is unavailable");
            return NULL;
        }
        state = ns_calloc(1u, sizeof(ScanState));
        state->refs = 1u;
        state->batchApi = batchApi;
        state->interp = interp;
        state->utf8 = Tcl_GetEncoding(interp, "utf-8");
        for (size_t i = 0u; i < N_FIELDS; i++) {
            state->names[i] = NsAtomObj(schema[i].name);
            Tcl_IncrRefCount(state->names[i]);
        }
        for (size_t i = 0u; i < N_TYPES; i++) {
            state->types[i] = fields[indexes[i]];
            Tcl_IncrRefCount(state->types[i]);
        }
        state->nullValue = fields[11];
        Tcl_IncrRefCount(state->nullValue);
        state->trueValue = fields[8];
        Tcl_IncrRefCount(state->trueValue);
        state->falseValue = fields[20];
        Tcl_IncrRefCount(state->falseValue);
        state->schema = SchemaInfo(state);
        Tcl_IncrRefCount(state->schema);
        state->batchSchema = batchApi->schemaCreate(interp, state->schema);
        if (state->batchSchema == NULL) {
            StateRelease(state);
            return NULL;
        }
        Tcl_SetAssocData(interp, "nslog:scan", StateDelete, state);
        Tcl_ResetResult(interp);
    }
    return state;
}

/*
 *----------------------------------------------------------------------
 *
 * Space --
 *
 *      Recognize access-log field separator bytes.
 *
 * Results:
 *      True for a space or horizontal tab.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
Space(char c)
{
    return c == ' ' || c == '\t';
}

/*
 *----------------------------------------------------------------------
 *
 * Decimal --
 *
 *      Validate a number lexeme with the shared JSON scanner. Integer fields
 *      additionally exclude fraction and exponent syntax.
 *
 * Results:
 *      True for the accepted numeric grammar.
 *
 * Side effects:
 *      None; the original lexical bytes are preserved.
 *
 *----------------------------------------------------------------------
 */
static bool
Decimal(Slice value, bool integer)
{
    bool lexicalInteger;

    return NsJsonNumberIsValid((const unsigned char *)value.bytes, value.length, &lexicalInteger)
           && (!integer || lexicalInteger);
}

/*
 *----------------------------------------------------------------------
 *
 * Text --
 *
 *      Create a present slice over a NUL-terminated string.
 *
 * Results:
 *      Borrowed slice.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Slice
Text(const char *s)
{
    Slice v = {s, strlen(s), NS_TRUE};

    return v;
}

/*
 *----------------------------------------------------------------------
 *
 * Part --
 *
 *      Create a present slice over length-delimited bytes.
 *
 * Results:
 *      Borrowed slice.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Slice
Part(const char *s, size_t n)
{
    Slice v = {s, n, NS_TRUE};

    return v;
}

/*
 *----------------------------------------------------------------------
 *
 * Equal --
 *
 *      Compare a present slice with a fixed string.
 *
 * Results:
 *      True for equal byte sequences.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
Equal(Slice s, const char *v)
{
    return s.present && s.length == strlen(v) && memcmp(s.bytes, v, s.length) == 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tokenize --
 *
 *      Split an access-log line while decoding quoted and escaped fields.
 *
 * Results:
 *      NULL on success, or a diagnostic string.
 *
 * Side effects:
 *      Writes token descriptors and appends decoded bytes to scratch storage.
 *
 *----------------------------------------------------------------------
 */
static const char *
Tokenize(const char *line, size_t length, Token *tokens, size_t *count, Tcl_DString *decoded)
{
    size_t i = 0u;
    *count = 0u;
    while (i < length) {
        Token *token;

        while (i < length && Space(line[i])) {
            i++;
        }
        if (i == length) {
            break;
        }
        if (*count == SCAN_TOKENS) {
            return "too-many-fields";
        }
        token = &tokens[(*count)++];
        token->rawStart = i;
        token->offset = (size_t)decoded->length;
        token->kind = line[i] == '"' ? 'q' : line[i] == '[' ? 't' : 'b';
        if (token->kind == 'b') {
            size_t start = i;

            while (i < length && !Space(line[i])) {
                i++;
            }
            Tcl_DStringAppend(decoded, line + start, (TCL_SIZE_T)(i - start));
        } else {
            char end = token->kind == 'q' ? '"' : ']';
            bool closed = NS_FALSE;

            i++;
            while (i < length) {
                char c = line[i++];

                if (c == end) {
                    closed = NS_TRUE;
                    break;
                }
                if (c == '\\' && token->kind == 'q') {
                    if (i == length) {
                        return "unfinished-escape";
                    }
                    c = line[i++];
                    switch (c) {
                    case 'n':
                        c = '\n';
                        break;
                    case 'r':
                        c = '\r';
                        break;
                    case 't':
                        c = '\t';
                        break;
                    case '\\':
                    case '"':
                        break;
                    default:
                        return "unsupported-escape";
                    }
                }
                Tcl_DStringAppend(decoded, &c, 1);
            }
            if (!closed || (i < length && !Space(line[i]))) {
                return "field-delimiter";
            }
        }
        token->length = (size_t)decoded->length - token->offset;
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Timing --
 *
 *      Split a timing tuple into its lexical phase fields.
 *
 * Results:
 *      True when the tuple is syntactically valid.
 *
 * Side effects:
 *      Initializes the supplied phase slices.
 *
 *----------------------------------------------------------------------
 */
static bool
Timing(Slice value, Slice parts[5])
{
    size_t i = 0u;

    for (size_t n = 0u; n < 5u; n++) {
        size_t start;

        while (i < value.length && Space(value.bytes[i])) {
            i++;
        }
        start = i;
        while (i < value.length && !Space(value.bytes[i])) {
            i++;
        }
        parts[n] = Part(value.bytes + start, i - start);
        if (!Decimal(parts[n], NS_FALSE)) {
            return NS_FALSE;
        }
    }
    while (i < value.length && Space(value.bytes[i])) {
        i++;
    }
    return i == value.length;
}

/*
 *----------------------------------------------------------------------
 *
 * Identity --
 *
 *      Extract request source, driver and pool identity from its log token.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates the supplied row slices.
 *
 *----------------------------------------------------------------------
 */
static void
Identity(Slice id, Slice values[N_FIELDS])
{
    values[F_SOURCE] = Text("unknown");
    values[F_POOL] = Text("unknown");
    if (id.length > 8u && id.bytes[id.length - 1u] == '-') {
        if (memcmp(id.bytes, "-conn:", 6u) == 0) {
          /*
           * server:pool:thread:request; do not mine IDs from request text.
           */
            size_t colon[3], count = 0u;

            for (size_t i = 6u; i < id.length - 1u && count < 3u; i++) {
                if (id.bytes[i] == ':') {
                    colon[count++] = i;
                }
            }
            if (count == 3u && colon[0] > 6u && colon[1] > colon[0] + 1u
                && Decimal(Part(id.bytes + colon[1] + 1u, colon[2] - colon[1] - 1u), NS_TRUE)
                && Decimal(Part(id.bytes + colon[2] + 1u, id.length - colon[2] - 2u), NS_TRUE)) {
                values[F_SOURCE] = Text("connection");
                values[F_REQUESTID] = id;
                values[F_POOL] = Part(id.bytes + colon[0] + 1u, colon[1] - colon[0] - 1u);
            }
        } else if (memcmp(id.bytes, "-driver:", 8u) == 0) {
            size_t colon = 8u, dash;

            while (colon < id.length - 1u && id.bytes[colon] != ':') {
                colon++;
            }
            dash = colon + 1u;
            while (dash < id.length - 1u && id.bytes[dash] != '-') {
                dash++;
            }
            if (colon > 8u && colon < id.length - 1u
                && Decimal(Part(id.bytes + colon + 1u, dash - colon - 1u), NS_TRUE)) {
                values[F_SOURCE] = Text("driver");
                values[F_DRIVER] = Part(id.bytes + 8u, colon - 8u);
                if (dash < id.length - 1u) {
                    values[F_POOL] = Part(id.bytes + dash + 1u, id.length - dash - 2u);
                }
            }
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Timestamp --
 *
 *      Check the lexical structure of an access-log timestamp.
 *
 * Results:
 *      True for a recognized timestamp shape.
 *
 * Side effects:
 *      None; calendar and database conversion remain sink responsibilities.
 *
 *----------------------------------------------------------------------
 */
static bool
Timestamp(Slice value)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    bool              month = NS_FALSE;

    if (Decimal(value, NS_TRUE)) {
        return NS_TRUE;
    }
    if (value.length != 26u || value.bytes[2] != '/' || value.bytes[6] != '/'
        || value.bytes[11] != ':' || value.bytes[14] != ':' || value.bytes[17] != ':'
        || value.bytes[20] != ' ' || (value.bytes[21] != '+' && value.bytes[21] != '-')) {
        return NS_FALSE;
    }
    for (size_t i = 0u; i < 12u; i++) {
        if (memcmp(value.bytes + 3u, months + i * 3u, 3u) == 0) {
            month = NS_TRUE;
            break;
        }
    }
    if (!month) {
        return NS_FALSE;
    }
    for (size_t i = 0u; i < value.length; i++) {
        if (i == 2u || (i >= 3u && i <= 6u) || i == 11u || i == 14u || i == 17u || i == 20u
            || i == 21u) {
            continue;
        }
        if (value.bytes[i] < '0' || value.bytes[i] > '9') {
            return NS_FALSE;
        }
    }
    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * Parse --
 *
 *      Parse one physical log record into schema-associated lexical slices.
 *
 * Results:
 *      NULL on success, or a diagnostic string.
 *
 * Side effects:
 *      Updates row slices and decoded scratch storage.
 *
 *----------------------------------------------------------------------
 */
static const char *
Parse(Scan *scan, Slice values[N_FIELDS], Tcl_DString *decoded)
{
    Token       tokens[SCAN_TOKENS];
    size_t      count, length = scan->lineLength;
    const char *line = scan->line, *error, *first, *last = NULL;
    Slice       fields[SCAN_TOKENS], request, phases[5];
    size_t      tail = 7u, dataEnd;
    bool        combined = scan->format == 2;

    scan->line[length] = '\0';
    if (length > 0u && line[length - 1u] == '\r') {
        length--;
    }
    if (memchr(line, 0, length) != NULL
        || !Ns_Valid_UTF8((const unsigned char *)line, length, NULL)) {
        return "encoding";
    }
    error = Tokenize(line, length, tokens, &count, decoded);
    if (error != NULL) {
        return error;
    }
    if (count < 7u || tokens[0].kind != 'b' || tokens[1].kind != 'b' || tokens[3].kind != 't'
        || tokens[4].kind != 'q' || tokens[5].kind != 'b' || tokens[6].kind != 'b') {
        return "layout";
    }
    for (size_t i = 0u; i < count; i++) {
        fields[i] = Part(decoded->string + tokens[i].offset, tokens[i].length);
    }
    if (!Timestamp(fields[3])) {
        return "timestamp-syntax";
    }
    if (!Decimal(fields[5], NS_TRUE) || (!Equal(fields[6], "-") && !Decimal(fields[6], NS_TRUE))) {
        return "numeric-syntax";
    }
    values[F_TIMESTAMP] = fields[3];
    values[F_PEER] = fields[0];
    values[F_THREAD] = fields[1];
    values[F_USER] = fields[2];
    values[F_REQUEST] = fields[4];
    values[F_STATUS] = fields[5];
    if (!Equal(fields[6], "-")) {
        values[F_BYTES] = fields[6];
    }
    Identity(fields[1], values);
    request = fields[4];
    first = memchr(request.bytes, ' ', request.length);
    if (first != NULL) {
        for (size_t i = request.length; i > 0u; i--) {
            if (request.bytes[i - 1u] == ' ') {
                last = request.bytes + i - 1u;
                break;
            }
        }
    }
    if (first != NULL && first > request.bytes && last > first
        && memchr(last + 1, '/', request.length - (size_t)(last + 1 - request.bytes)) != NULL) {
        values[F_METHOD] = Part(request.bytes, (size_t)(first - request.bytes));
        values[F_TARGET] = Part(first + 1, (size_t)(last - first - 1));
        values[F_PROTOCOL] = Part(last + 1, request.length - (size_t)(last + 1 - request.bytes));
    } else if (request.length > 0u && request.bytes[0] == '/') {
        values[F_TARGET] = request;
    }
    if (values[F_TARGET].present) {
        Slice       target = values[F_TARGET];
        const char *query = memchr(target.bytes, '?', target.length);
        size_t      length = query == NULL ? target.length : (size_t)(query - target.bytes);
        const char *path = target.bytes;

        for (size_t i = 1u; i + 2u < length; i++) {
            if (target.bytes[i] == ':' && target.bytes[i + 1u] == '/'
                && target.bytes[i + 2u] == '/') {
                bool scheme = (target.bytes[0] >= 'a' && target.bytes[0] <= 'z')
                              || (target.bytes[0] >= 'A' && target.bytes[0] <= 'Z');

                for (size_t n = 1u; n < i && scheme; n++) {
                    char c = target.bytes[n];

                    scheme = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                             || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
                }
                if (!scheme) {
                    break;
                }
                path = memchr(target.bytes + i + 3u, '/', length - i - 3u);
                if (path == NULL) {
                    values[F_PATH] = Text("/");
                } else {
                    values[F_PATH] = Part(path, length - (size_t)(path - target.bytes));
                }
                break;
            }
        }
        if (!values[F_PATH].present) {
            values[F_PATH] = Part(path, length);
        }
    }
    dataEnd = count;
    if (scan->extendedHeaders != NULL) {
        if (count < 7u + (size_t)scan->headerCount) {
            return "extended-header-layout";
        }
        dataEnd -= (size_t)scan->headerCount;
        for (size_t i = dataEnd; i < count; i++) {
            if (tokens[i].kind != 'q') {
                return "extended-header-layout";
            }
        }
    }
    if (scan->format == 0 && dataEnd >= 9u && tokens[7].kind == 'q' && tokens[8].kind == 'q'
        && !Timing(fields[7], phases)) {
        combined = NS_TRUE;
    }
    if (combined) {
        if (dataEnd < 9u || tokens[7].kind != 'q' || tokens[8].kind != 'q') {
            return "combined-layout";
        }
        values[F_REFERER] = fields[7];
        values[F_AGENT] = fields[8];
        tail = 9u;
    }
    if (scan->requestTime != 0 && tail < dataEnd && tokens[tail].kind == 'b'
        && Decimal(fields[tail], NS_FALSE)) {
        values[F_REQTIME] = fields[tail++];
    } else if (scan->requestTime == 1) {
        return "request-time-layout";
    }
    if (scan->timing != -2) {
        size_t quoted = 0u;

        for (size_t i = 7u; i < dataEnd; i++) {
            bool candidate;

            if (tokens[i].kind != 'q') {
                continue;
            }
            candidate = scan->timing == -1 ? i == tail : quoted == (size_t)scan->timing;
            if (candidate && Timing(fields[i], phases)) {
                values[F_START] = phases[0];
                if (!Equal(values[F_SOURCE], "driver")) {
                    values[F_ACCEPT] = phases[1];
                    values[F_QUEUE] = phases[2];
                    values[F_FILTER] = phases[3];
                    values[F_RUN] = phases[4];
                    values[F_PHASE] = Text("true");
                    values[F_REASON] = Text("logged-worker-phases");
                }
                if (i == tail) {
                    tail++;
                }
                break;
            }
            quoted++;
        }
    }
    if (scan->extendedHeaders != NULL && tail != dataEnd) {
        return "extended-header-layout";
    }
    if (tail < count) {
        values[F_EXTRA] = Part(line + tokens[tail].rawStart, length - tokens[tail].rawStart);
    }
    if (!values[F_PHASE].present) {
        values[F_PHASE] = Text("false");
        values[F_REASON] = Text(Equal(values[F_SOURCE], "driver") ? "driver-synthetic-phases"
                                                                  : "no-phase-counters");
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * ReadAt --
 *
 *      Read source bytes at a fixed position without changing scanner
 *      progress.
 *
 * Results:
 *      NS_OK if all requested bytes were read and the position restored;
 *      NS_ERROR otherwise.
 *
 * Side effects:
 *      Temporarily seeks and reads the source, then restores its position.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ReadAt(Scan *scan, Tcl_WideInt offset, unsigned char *buffer, size_t length)
{
    off_t         saved = ns_lseek(scan->fd, 0, SEEK_CUR);
    size_t        done = 0u;
    Ns_ReturnCode result = NS_ERROR;

    if (saved < 0 || ns_lseek(scan->fd, (off_t)offset, SEEK_SET) < 0) {
        return NS_ERROR;
    }
    while (done < length) {
        ssize_t n = ns_read(scan->fd, buffer + done, length - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            goto finish;
        }
        done += (size_t)n;
    }
    result = NS_OK;
finish:
    if (ns_lseek(scan->fd, saved, SEEK_SET) < 0) {
        result = NS_ERROR;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SourceInfo --
 *
 *      Describe the source identity and snapshot boundary.
 *
 * Results:
 *      New dictionary.
 *
 * Side effects:
 *      Allocates metadata objects using core atom keys.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
SourceInfo(const Scan *scan)
{
    Tcl_Obj *info = Tcl_NewDictObj();

    StringPut(info, NS_ATOM_filename, scan->filename);
    WidePut(info, NS_ATOM_device, scan->device);
    WidePut(info, NS_ATOM_inode, scan->inode);
    WidePut(info, NS_ATOM_snapshotEnd, scan->snapshotEnd);
    if (scan->extendedHeaders != NULL) {
        DictPut(info, NS_ATOM_extraheaders, scan->extendedHeaders);
    }
    return info;
}

/*
 *----------------------------------------------------------------------
 *
 * Checkpoint --
 *
 *      Capture a resumable position with source boundary fingerprints.
 *
 * Results:
 *      New dictionary, or NULL when the boundary cannot be read.
 *
 * Side effects:
 *      Reads boundary bytes without advancing the scanner.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
Checkpoint(Scan *scan)
{
    unsigned char anchor[SCAN_PREFIX];
    Tcl_Obj      *checkpoint;
    size_t        length = scan->safeOffset < SCAN_PREFIX ? (size_t)scan->safeOffset : SCAN_PREFIX;

    if (ReadAt(scan, scan->safeOffset - (Tcl_WideInt)length, anchor, length) != NS_OK) {
        return NULL;
    }
    checkpoint = Tcl_NewDictObj();
    WidePut(checkpoint, NS_ATOM_version, 1);
    DictPut(checkpoint, NS_ATOM_producer, NsAtomObj(NS_ATOM_accesslog));
    WidePut(checkpoint, NS_ATOM_device, scan->device);
    WidePut(checkpoint, NS_ATOM_inode, scan->inode);
    WidePut(checkpoint, NS_ATOM_offset, scan->safeOffset);
    DictPut(checkpoint, NS_ATOM_prefix,
            Tcl_NewByteArrayObj(scan->prefix, (TCL_SIZE_T)scan->prefixLength));
    DictPut(checkpoint, NS_ATOM_anchor, Tcl_NewByteArrayObj(anchor, (TCL_SIZE_T)length));
    WidePut(checkpoint, NS_ATOM_format, scan->format);
    WidePut(checkpoint, NS_ATOM_timingColumn, scan->timing);
    WidePut(checkpoint, NS_ATOM_maxLineBytes, (Tcl_WideInt)scan->maxLine);
    WidePut(checkpoint, NS_ATOM_final, scan->final);
    WidePut(checkpoint, NS_ATOM_requestTime, scan->requestTime);
    if (scan->extendedHeaders != NULL) {
        DictPut(checkpoint, NS_ATOM_extraheaders, scan->extendedHeaders);
    }
    return checkpoint;
}

/*
 *----------------------------------------------------------------------
 *
 * GetKey --
 *
 *      Look up checkpoint metadata using a core atom key.
 *
 * Results:
 *      Borrowed value, or NULL when absent or on dictionary error.
 *
 * Side effects:
 *      May set the interpreter error result.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
GetKey(Tcl_Interp *interp, Tcl_Obj *dict, NsAtomId key)
{
    Tcl_Obj *name = NsAtomObj(key), *value = NULL;

    Tcl_IncrRefCount(name);
    if (Tcl_DictObjGet(interp, dict, name, &value) != TCL_OK) {
        value = NULL;
    }
    Tcl_DecrRefCount(name);
    return value;
}

/*
 *----------------------------------------------------------------------
 *
 * Resume --
 *
 *      Validate a checkpoint against the source snapshot and scanner options.
 *
 * Results:
 *      TCL_OK with the resume offset, or TCL_ERROR.
 *
 * Side effects:
 *      Reads source boundary bytes and may set the interpreter error result.
 *
 *----------------------------------------------------------------------
 */
static int
Resume(Tcl_Interp *interp, Scan *scan, Tcl_Obj *checkpoint, Tcl_WideInt *offset)
{
    Tcl_Obj       *producer, *position;
    const NsAtomId names[] = {NS_ATOM_version, NS_ATOM_device,       NS_ATOM_inode,
                              NS_ATOM_format,  NS_ATOM_timingColumn, NS_ATOM_maxLineBytes,
                              NS_ATOM_final, NS_ATOM_requestTime};
    Tcl_WideInt    expected[] = {1,
                                 scan->device,
                                 scan->inode,
                                 scan->format,
                                 scan->timing,
                                 (Tcl_WideInt)scan->maxLine,
                                 scan->final, scan->requestTime};

    for (size_t i = 0u; i < Ns_NrElements(names); i++) {
        Tcl_Obj    *obj = GetKey(interp, checkpoint, names[i]);
        Tcl_WideInt value;

        if (obj == NULL || Tcl_GetWideIntFromObj(interp, obj, &value) != TCL_OK
            || value != expected[i]) {
            goto mismatch;
        }
    }
    {
        Tcl_Obj *headers = GetKey(interp, checkpoint, NS_ATOM_extraheaders);

        if ((headers == NULL) != (scan->extendedHeaders == NULL)
            || (headers != NULL && strcmp(Tcl_GetString(headers),
                                          Tcl_GetString(scan->extendedHeaders)) != 0)) {
            goto mismatch;
        }
    }
    producer = GetKey(interp, checkpoint, NS_ATOM_producer);
    position = GetKey(interp, checkpoint, NS_ATOM_offset);
    if (producer == NULL || strcmp(Tcl_GetString(producer), "accesslog") != 0 || position == NULL
        || Tcl_GetWideIntFromObj(interp, position, offset) != TCL_OK || *offset < 0
        || *offset > scan->snapshotEnd) {
        goto mismatch;
    }
    for (int i = 0; i < 2; i++) {
        Tcl_Obj      *obj = GetKey(interp, checkpoint, i == 0 ? NS_ATOM_prefix : NS_ATOM_anchor);
        TCL_SIZE_T    length;
        unsigned char buffer[SCAN_PREFIX], *bytes;
        Tcl_WideInt   start;

        if (obj == NULL) {
            goto mismatch;
        }
        bytes = Tcl_GetByteArrayFromObj(obj, &length);
        if (length < 0 || (size_t)length > SCAN_PREFIX
            || (i == 1 && length != (*offset < SCAN_PREFIX ? *offset : SCAN_PREFIX))) {
            goto mismatch;
        }
        start = i == 0 ? 0 : *offset - length;
        if (start + length > scan->snapshotEnd
            || ReadAt(scan, start, buffer, (size_t)length) != NS_OK
            || memcmp(buffer, bytes, (size_t)length) != 0) {
            goto mismatch;
        }
    }
    return TCL_OK;
mismatch:
    return ScanError(interp, "CHECKPOINT_MISMATCH",
                     "checkpoint does not match the source or scanner configuration");
}

/*
 *----------------------------------------------------------------------
 *
 * NormalizeHeaders --
 *
 *      Normalize header names in the logger's request-then-response order.
 *      Plain names denote request headers; tagged names use logger prefixes.
 *
 * Results:
 *      Owned list of request:/response: names, or NULL with a Tcl error.
 *
 * Side effects:
 *      Allocates a list and may set the interpreter error result.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
NormalizeHeaders(Tcl_Interp *interp, Tcl_Obj *headers)
{
    Tcl_Obj **items, *result = Tcl_NewListObj(0, NULL);
    TCL_SIZE_T count;

    Tcl_IncrRefCount(result);
    if (Tcl_ListObjGetElements(interp, headers, &count, &items) != TCL_OK) {
        Tcl_DecrRefCount(result);
        return NULL;
    }
    for (int response = 0; response < 2; response++) {
        for (TCL_SIZE_T i = 0; i < count; i++) {
            TCL_SIZE_T nameLength;
            const char *name = Tcl_GetStringFromObj(items[i], &nameLength);
            const char *colon = strchr(name, ':');
            const char *field = colon == NULL ? name : colon + 1;
            bool isResponse = false;

            if (strlen(name) != (size_t)nameLength || strstr(name, "\xc0\x80") != NULL) {
                (void)ScanError(interp, "OPTION", "extended-header names must not contain NUL");
                Tcl_DecrRefCount(result);
                return NULL;
            }
            if (colon != NULL) {
                size_t length = (size_t)(colon - name);

                if (length >= 3u && length <= 8u && strncmp(name, "response", length) == 0) {
                    isResponse = true;
                } else if (!(length >= 3u && length <= 7u && strncmp(name, "request", length) == 0)) {
                    (void)ScanError(interp, "OPTION", "invalid extended-header prefix");
                    Tcl_DecrRefCount(result);
                    return NULL;
                }
            }
            if (*field == '\0') {
                (void)ScanError(interp, "OPTION", "extended-header names must not be empty");
                Tcl_DecrRefCount(result);
                return NULL;
            }
            if (isResponse == (response != 0)) {
                Tcl_ListObjAppendElement(NULL, result,
                                         Tcl_ObjPrintf("%s:%s", response ? "response" : "request", field));
            }
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * OpenScanObjCmnd --
 *
 *      This command implements "ns_accesslog scan open". It opens a bounded
 *      source snapshot using the requested scan options.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Opens the source and returns an opaque scanner in the interpreter
 *      result.
 *
 *----------------------------------------------------------------------
 */
static int
OpenScanObjCmnd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const *objv)
{
    const NsLogScanDefaults *defaults = clientData;
    const char         *filename = defaults->filename;
    const char         *timing = NULL, *tail = "pending", *encoding = "utf-8";
    Tcl_Obj            *checkpoint = NULL, *offsetObj = NULL, *headers = NULL;
    int                 maxLine = 65536, flags, format = -1, requestTime = -1;
    Tcl_WideInt         offset = 0;
    ScanState          *state;
    Scan               *scan;
    struct stat         st;
    unsigned char       previous;
    Ns_ObjvValueRange   lineRange = {128, 1048576};
    static Ns_ObjvTable formats[] = {
      {"auto", 0},
      {"common", 1},
      {"combined", 2},
      {NULL, 0}
    };
    static Ns_ObjvTable requestTimes[] = {
      {"auto", 0},
      {"true", 1},
      {"false", 2},
      {NULL, 0}
    };
    Ns_ObjvSpec         options[] = {
      {"-filename",        Ns_ObjvString, &filename,    NULL},
      {"-format",          Ns_ObjvIndex,  &format,      formats},
      {"-timingcolumn",    Ns_ObjvString, &timing,      NULL},
      {"-requesttime",     Ns_ObjvIndex,  &requestTime, requestTimes},
      {"-extendedheaders", Ns_ObjvObj,    &headers,     NULL},
      {"-tail",            Ns_ObjvString, &tail,        NULL},
      {"-encoding",        Ns_ObjvString, &encoding,    NULL},
      {"-offset",          Ns_ObjvObj,    &offsetObj,   NULL},
      {"-checkpoint",      Ns_ObjvObj,    &checkpoint,  NULL},
      {"-maxlinebytes",    Ns_ObjvInt,    &maxLine,     &lineRange},
      {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(options, NULL, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else if ((timing != NULL && strcmp(timing, "auto") != 0 && strcmp(timing, "none") != 0
         && strcmp(timing, "0") != 0 && strcmp(timing, "2") != 0)
        || (strcmp(tail, "pending") != 0 && strcmp(tail, "final") != 0)
        || strcmp(encoding, "utf-8") != 0) {
        return ScanError(interp, "OPTION",
                         "expected format auto/common/combined, timingcolumn auto/none/0/2, tail "
                         "pending/final and encoding utf-8");

    } else if (offsetObj != NULL && checkpoint != NULL) {
        return ScanError(interp, "OPTION", "-offset and -checkpoint are mutually exclusive");

    } else if (offsetObj != NULL && Tcl_GetWideIntFromObj(interp, offsetObj, &offset) != TCL_OK) {
        return TCL_ERROR;
    }

    state = GetState(interp);
    if (state == NULL) {
        return TCL_ERROR;
    }
    scan = ns_calloc(1u, sizeof(Scan));
    scan->refs = 1u;
    scan->owner = state;
    state->refs++;
    scan->next = state->scans;
    if (scan->next != NULL) {
        scan->next->prev = scan;
    }
    state->scans = scan;
    scan->fd = NS_INVALID_FD;
    scan->filename = ns_strdup(filename);
    scan->maxLine = (size_t)maxLine;

    scan->final = strcmp(tail, "final") == 0;
    flags = O_RDONLY | O_CLOEXEC | O_BINARY;
#ifdef O_NONBLOCK
    /* Reject FIFOs/devices without blocking before the regular-file check. */ flags |= O_NONBLOCK;
#endif
    scan->fd = ns_open(filename, flags, 0);
    if (scan->fd == NS_INVALID_FD || fstat(scan->fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ScanRelease(scan);
        return ScanError(interp, "OPEN", "cannot open a regular access-log file");
    }
    scan->device = (Tcl_WideInt)st.st_dev;
    scan->inode = (Tcl_WideInt)st.st_ino;
    scan->snapshotEnd = (Tcl_WideInt)st.st_size;
    {
        bool current = defaults->haveIdentity && scan->device == defaults->device
                       && scan->inode == defaults->inode;

        scan->format = format;
        if (format < 0) {
            scan->format = current ? (defaults->combined ? 2 : 1) : 0;
        }
        scan->requestTime = -1;
        if (requestTime >= 0) {
            scan->requestTime = requestTime == 0 ? -1 : requestTime == 1;
        } else if (current) {
            scan->requestTime = defaults->requestTime;
        }
        scan->timing = -1;
        if (timing != NULL) {
            if (strcmp(timing, "none") == 0) {
                scan->timing = -2;
            } else if (strcmp(timing, "auto") != 0) {
                scan->timing = timing[0] - '0';
            }
        } else if (current) {
            if (!defaults->partialTimes) {
                scan->timing = -2;
            } else if (scan->format != 0) {
                scan->timing = scan->format == 2 ? 2 : 0;
            }
        }
        if (headers == NULL && current) {
            headers = defaults->extendedHeaders;
        }
        if (headers != NULL && strcmp(Tcl_GetString(headers), "auto") != 0) {
            scan->extendedHeaders = NormalizeHeaders(interp, headers);
            if (scan->extendedHeaders == NULL) {
                ScanRelease(scan);
                return TCL_ERROR;
            }
            Tcl_ListObjLength(NULL, scan->extendedHeaders, &scan->headerCount);
        }
    }
    scan->prefixLength = scan->snapshotEnd < SCAN_PREFIX ? (size_t)scan->snapshotEnd : SCAN_PREFIX;
    if (ReadAt(scan, 0, scan->prefix, scan->prefixLength) != NS_OK) {
        ScanRelease(scan);
        return ScanError(interp, "SOURCE_CHANGED", "source changed while opening snapshot");
    }
    if (checkpoint != NULL && Resume(interp, scan, checkpoint, &offset) != TCL_OK) {
        ScanRelease(scan);
        return TCL_ERROR;
    }
    if (offset < 0 || offset > scan->snapshotEnd
        || (offset > 0 && !(offset == scan->snapshotEnd && scan->final)
            && (ReadAt(scan, offset - 1, &previous, 1u) != NS_OK || previous != '\n'))
        || ns_lseek(scan->fd, (off_t)offset, SEEK_SET) < 0) {
        ScanRelease(scan);
        return ScanError(interp, "BOUNDARY", "offset must be a snapshot record boundary");
    }
    scan->offset = scan->safeOffset = scan->lineStart = offset;
    scan->line = ns_malloc(scan->maxLine + 1u);
    Tcl_SetObjResult(interp, Wrap(scan, &scannerType));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SchemaInfo --
 *
 *      Build the shared access-log schema from core atom descriptors.
 *
 * Results:
 *      New schema dictionary.
 *
 * Side effects:
 *      Allocates column dictionaries and retains the referenced atom objects.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
SchemaInfo(ScanState *state)
{
    Tcl_Obj *result = Tcl_NewDictObj(), *columns = Tcl_NewListObj(0, NULL);

    DictPut(result, NS_ATOM_id, NsAtomObj(NS_ATOM_naviserver_DOT_accesslog_DOT_scan));
    WidePut(result, NS_ATOM_version, 1);
    for (size_t i = 0u; i < N_FIELDS; i++) {
        Tcl_Obj *column = Tcl_NewDictObj();

        DictPut(column, NS_ATOM_name, state->names[i]);
        DictPut(column, NS_ATOM_type, state->types[schema[i].type]);
        WidePut(column, NS_ATOM_nullable,
                i == F_OFFSET || i == F_END || i == F_PARSE || i == F_TRUNCATED || i == F_LINES
                    ? 0
                    : 1);
        if (schema[i].refinement >= 0) {
            DictPut(column, NS_ATOM_refinement, NsAtomObj(schema[i].refinement));
        }
        if (schema[i].representation >= 0) {
            DictPut(column, NS_ATOM_representation, NsAtomObj(schema[i].representation));
        }
        (void)Tcl_ListObjAppendElement(NULL, columns, column);
    }
    DictPut(result, NS_ATOM_columns, columns);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * AppendRow --
 *
 *      Append lexical row slices to compact batch-owned storage.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Grows row and arena storage and copies present field bytes.
 *
 *----------------------------------------------------------------------
 */
static void
AppendRow(Batch *batch, Slice values[N_FIELDS])
{
    Row *row;

    if (batch->count == batch->capacity) {
        batch->capacity = batch->capacity == 0u ? 16u : batch->capacity * 2u;
        batch->rows = ns_realloc(batch->rows, batch->capacity * sizeof(Row));
    }
    row = &batch->rows[batch->count++];
    memset(row, 0, sizeof(Row));
    for (size_t i = 0u; i < N_FIELDS; i++) {
        if (values[i].present) {
            row->present |= UINT64_C(1) << i;
            row->cells[i].offset = (uint32_t)batch->arena.length;
            row->cells[i].length = (uint32_t)values[i].length;
            Tcl_DStringAppend(&batch->arena, values[i].bytes, (TCL_SIZE_T)values[i].length);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ReadBatchValue --
 *
 *      Expose a compact batch cell as a borrowed lexical value.
 *
 * Results:
 *      NS_OK or NS_ERROR for an out-of-range cell.
 *
 * Side effects:
 *      Initializes the output value descriptor.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ReadBatchValue(const void *payload, size_t row, size_t field, Ns_DbBatchValue *value)
{
    const Batch *batch = payload;
    const Row   *r;

    if (row >= batch->count || field >= N_FIELDS) {
        return NS_ERROR;
    }
    r = &batch->rows[row];
    value->isNull = (r->present & (UINT64_C(1) << field)) == 0u;
    value->length = value->isNull ? 0u : r->cells[field].length;
    value->bytes =
        value->isNull ? NULL : (const unsigned char *)batch->arena.string + r->cells[field].offset;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ReleaseBatchPayload --
 *
 *      Release compact storage adopted by the shared batch API.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Drops the producer batch reference.
 *
 *----------------------------------------------------------------------
 */
static void
ReleaseBatchPayload(void *payload)
{
    BatchRelease(payload);
}

/*
 *----------------------------------------------------------------------
 *
 * NextScan --
 *
 *      Scan up to the row, byte and time budgets into a compact batch.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Advances source progress and returns an opaque batch with its
 *      checkpoint.
 *
 *----------------------------------------------------------------------
 */
static int
NextScan(Tcl_Interp *interp, Scan *scan, TCL_SIZE_T objc, Tcl_Obj *const *objv)
{
    int               maxRows = 512, maxBytes = 8388608, maxScan = 16777216;
    Ns_Time           defaultTimeout = {0, 100000}, started, now, *timeout = &defaultTimeout;
    Ns_ObjvValueRange rowRange = {1, 1000000}, byteRange = {1, 1073741824};
    Ns_ObjvTimeRange  timeRange = {{0, 1}, {3600, 0}};
    struct stat       st;
    Batch            *batch;
    size_t            reserve = scan->maxLine * 4u + 2048u, malformed = 0u, oversized = 0u;
    Tcl_WideInt       from = scan->safeOffset, examined = 0;
    NsAtomId          state = NS_ATOM_ready;
    Tcl_Obj          *checkpoint, *coverage, *diagnostics, *view;
    Ns_ObjvSpec       options[] = {
      {"-maxrecords",   Ns_ObjvInt,  &maxRows,  &rowRange},
      {"-maxbytes",     Ns_ObjvInt,  &maxBytes, &byteRange},
      {"-maxscanbytes", Ns_ObjvInt,  &maxScan,  &byteRange},
      {"-timeout",      Ns_ObjvTime, &timeout,  &timeRange},
      {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(options, NULL, interp, 4, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (scan->fd == NS_INVALID_FD) {
        return ScanError(interp, "CLOSED", "scanner is closed");
    } /* Reserve the worst retained row, including derived request/path fields. */
    if ((size_t)maxBytes < reserve) {
        return ScanError(interp, "LIMIT",
                         "-maxbytes must accommodate 4 * maxlinebytes + 2048 bytes");
    }
    if (fstat(scan->fd, &st) != 0 || (Tcl_WideInt)st.st_size < scan->snapshotEnd) {
        return ScanError(interp, "SOURCE_CHANGED",
                         "snapshot source was truncated or became unavailable");
    }
    batch = ns_calloc(1u, sizeof(Batch));
    batch->refs = 1u;
    batch->owner = scan->owner;
    batch->owner->refs++;
    Tcl_DStringInit(&batch->arena);
    batch->metadata = Tcl_NewDictObj();
    Tcl_IncrRefCount(batch->metadata);
    Ns_GetTime(&started);
    for (;;) {
        Slice       values[N_FIELDS] = {{0}};
        Tcl_DString decoded;
        const char *error;
        char        start[32], stop[32], excerpt[SCAN_EXCERPT * 2u];

        if (scan->offset == scan->snapshotEnd) {
            if (scan->offset > scan->lineStart && !scan->final) {
                state = NS_ATOM_pending;
                break;
            }
            if (scan->offset == scan->lineStart) {
                state = NS_ATOM_snapshot_DASH_end;
                break;
            } /* -tail final accounts for the last unterminated physical line. */
        } else {
            char c;

            if (batch->count == (size_t)maxRows
                || (size_t)batch->arena.length + reserve > (size_t)maxBytes) {
                break;
            }
            if (examined >= maxScan) {
                state = NS_ATOM_budget;
                break;
            }
            if ((examined & 4095) == 0) {
                Tcl_WideInt usec;

                Ns_GetTime(&now);
                usec = (Tcl_WideInt)(now.sec - started.sec) * 1000000 + now.usec - started.usec;
                if (usec >= (Tcl_WideInt)timeout->sec * 1000000 + timeout->usec) {
                    state = NS_ATOM_budget;
                    break;
                }
            }
            if (scan->bufferPos == scan->bufferLength) {
                size_t  want = scan->snapshotEnd - scan->offset < SCAN_BUFFER
                                   ? (size_t)(scan->snapshotEnd - scan->offset)
                                   : SCAN_BUFFER;

                ssize_t n;
                do {
                    n = ns_read(scan->fd, scan->buffer, want);
                } while (n < 0 && errno == EINTR);
                if (n <= 0) {
                    BatchRelease(batch);
                    return ScanError(interp, "SOURCE_CHANGED",
                                     "snapshot ended before its captured size");
                }
                scan->bufferPos = 0u;
                scan->bufferLength = (size_t)n;
            }
            c = (char)scan->buffer[scan->bufferPos++];
            scan->offset++;
            examined++;
            if (c != '\n') {
                if (scan->lineLength < scan->maxLine) {
                    scan->line[scan->lineLength++] = c;
                } else {
                    scan->oversized = NS_TRUE;
                }
                continue;
            }
        }
        Tcl_DStringInit(&decoded);
        error = scan->oversized ? "oversized" : Parse(scan, values, &decoded);
        (void)snprintf(start, sizeof(start), "%" PRId64, (int64_t)scan->lineStart);
        (void)snprintf(stop, sizeof(stop), "%" PRId64, (int64_t)scan->offset);
        if (error != NULL) {
            size_t            n = scan->lineLength < SCAN_EXCERPT ? scan->lineLength : SCAN_EXCERPT;
            static const char hex[] = "0123456789abcdef";

            memset(values, 0, sizeof(values));
            values[F_PARSE] = Text("diagnostic");
            values[F_DIAGNOSTIC] = Text(error);
            for (size_t i = 0u; i < n; i++) {
                unsigned char c = (unsigned char)scan->line[i];

                excerpt[i * 2u] = hex[c >> 4u];
                excerpt[i * 2u + 1u] = hex[c & 15u];
            }
            values[F_EXCERPT] = Part(excerpt, n * 2u);
            if (scan->oversized) {
                oversized++;
            } else {
                malformed++;
            }
        } else {
            values[F_PARSE] = Text("ok");
        }
        values[F_OFFSET] = Text(start);
        values[F_END] = Text(stop);
        values[F_TRUNCATED] = Text(scan->oversized ? "true" : "false");
        values[F_LINES] = Text("1");
        AppendRow(batch, values);
        Tcl_DStringFree(&decoded);
        scan->safeOffset = scan->lineStart = scan->offset;
        scan->lineLength = 0u;
        scan->oversized = NS_FALSE;
        if (batch->count == (size_t)maxRows) {
            if (scan->offset == scan->snapshotEnd) {
                state = NS_ATOM_snapshot_DASH_end;
            }
            break;
        }
    }
    checkpoint = Checkpoint(scan);
    if (checkpoint == NULL) {
        BatchRelease(batch);
        return ScanError(interp, "SOURCE_CHANGED", "cannot validate checkpoint boundary");
    }
    WidePut(batch->metadata, NS_ATOM_batchVersion, 1);
    DictPut(batch->metadata, NS_ATOM_representation,
            NsAtomObj(NS_ATOM_compact_DASH_schema_DASH_lexical));
    DictPut(batch->metadata, NS_ATOM_producer, NsAtomObj(NS_ATOM_accesslog));
    view = NsAtomObj(NS_ATOM_typed_DASH_triples_DASH_lexical);
    DictPut(batch->metadata, NS_ATOM_views, Tcl_NewListObj(1, &view));
    DictPut(batch->metadata, NS_ATOM_schema, scan->owner->schema);
    WidePut(batch->metadata, NS_ATOM_rows, (Tcl_WideInt)batch->count);
    WidePut(batch->metadata, NS_ATOM_payloadBytes, batch->arena.length);
    WidePut(batch->metadata, NS_ATOM_storageBytes,
            (Tcl_WideInt)(sizeof(Batch) + batch->capacity * sizeof(Row)
                          + (batch->arena.string == batch->arena.staticSpace
                                 ? 0u
                                 : (size_t)batch->arena.spaceAvl)));
    DictPut(batch->metadata, NS_ATOM_source, SourceInfo(scan));
    DictPut(batch->metadata, NS_ATOM_checkpoint, checkpoint);
    coverage = Tcl_NewDictObj();
    diagnostics = Tcl_NewDictObj();
    WidePut(coverage, NS_ATOM_from, from);
    WidePut(coverage, NS_ATOM_to, scan->safeOffset);
    WidePut(coverage, NS_ATOM_snapshotEnd, scan->snapshotEnd);
    WidePut(diagnostics, NS_ATOM_malformed, (Tcl_WideInt)malformed);
    WidePut(diagnostics, NS_ATOM_oversized, (Tcl_WideInt)oversized);
    DictPut(batch->metadata, NS_ATOM_coverage, coverage);
    DictPut(batch->metadata, NS_ATOM_diagnostics, diagnostics);
    DictPut(batch->metadata, NS_ATOM_state, NsAtomObj(state));
    {
        Tcl_Obj *object =
            scan->owner->batchApi->adopt(interp, scan->owner->batchSchema, batch->metadata,
                                         batch->count, batch, ReadBatchValue, ReleaseBatchPayload);

        if (object == NULL) {
            BatchRelease(batch);
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, object);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Triples --
 *
 *      Parse view options and materialize a bounded JSON-triples batch view.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Sets the interpreter result through the shared batch API.
 *
 *----------------------------------------------------------------------
 */
static int
Triples(Tcl_Interp *interp, const Ns_DbBatchApi *api, const Ns_DbBatch *batch, TCL_SIZE_T objc,
        Tcl_Obj *const *objv)
{
    int               first = 0, limit = 512;
    Ns_ObjvValueRange range = {0, INT_MAX};
    Ns_ObjvSpec       options[] = {
      {"-offset", Ns_ObjvInt, &first, &range},
      {"-limit",  Ns_ObjvInt, &limit, &range},
      {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(options, NULL, interp, 4, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    return api->triples(interp, batch, (size_t)first, (size_t)limit);
}

/*
 *----------------------------------------------------------------------
 *
 * GetScan --
 *
 *      Resolve a scanner owned by the current interpreter.
 *
 * Results:
 *      Borrowed scanner, or NULL with a Tcl error result.
 *
 * Side effects:
 *      May set the interpreter error result.
 *
 *----------------------------------------------------------------------
 */
static Scan *
GetScan(Tcl_Interp *interp, Tcl_Obj *obj)
{
    Scan *scan;

    if (obj->typePtr != &scannerType) {
        (void)ScanError(interp, "HANDLE",
                        "expected an opaque scanner handle, not its string label");
        return NULL;
    }
    scan = obj->internalRep.twoPtrValue.ptr1;
    if (scan->owner->interp != interp) {
        (void)ScanError(interp, "OWNER", "scanner belongs to another interpreter");
        return NULL;
    }
    return scan;
}

/*
 *----------------------------------------------------------------------
 *
 * ScanNextObjCmd --
 *
 *      This command implements "ns_accesslog scan next". It validates the
 *      scanner argument and produces the next bounded batch.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Advances source progress and sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
ScanNextObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc,
               Tcl_Obj *const *objv)
{
    Tcl_Obj    *obj;
    Scan       *scan;
    TCL_SIZE_T  remain;
    Ns_ObjvSpec args[] = {
      {"handle",  Ns_ObjvObj,  &obj,    NULL},
      {"?option", Ns_ObjvArgs, &remain, NULL},
      {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    scan = GetScan(interp, obj);
    if (scan == NULL) {
        return TCL_ERROR;
    }
    return NextScan(interp, scan, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * ScanInfoObjCmd --
 *
 *      This command implements "ns_accesslog scan info". It returns source or
 *      shared batch metadata.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Sets the interpreter result; source metadata is newly allocated.
 *
 *----------------------------------------------------------------------
 */
static int
ScanInfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc,
               Tcl_Obj *const *objv)
{
    Tcl_Obj    *obj, *info;
    Ns_ObjvSpec args[] = {
      {"handle", Ns_ObjvObj, &obj, NULL},
      {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (obj->typePtr == &scannerType) {
        Scan *scan = GetScan(interp, obj);

        if (scan == NULL) {
            return TCL_ERROR;
        }
        info = SourceInfo(scan);
        WidePut(info, NS_ATOM_readOffset, scan->offset);
        WidePut(info, NS_ATOM_safeOffset, scan->safeOffset);
        DictPut(info, NS_ATOM_state,
                NsAtomObj(scan->fd == NS_INVALID_FD           ? NS_ATOM_closed
                          : scan->offset == scan->snapshotEnd ? scan->offset > scan->lineStart
                                                                    ? NS_ATOM_pending
                                                                    : NS_ATOM_snapshot_DASH_end
                                                              : NS_ATOM_ready));
    } else {
        ScanState        *state = GetState(interp);
        const Ns_DbBatch *batch;

        if (state == NULL || state->batchApi->get(interp, obj, &batch) != TCL_OK) {
            return TCL_ERROR;
        }
        info = state->batchApi->info(batch);
    }
    Tcl_SetObjResult(interp, info);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ScanCloseObjCmd --
 *
 *      This command implements "ns_accesslog scan close". It closes the
 *      source associated with an opaque scanner.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Closes the file descriptor; repeated closes are harmless.
 *
 *----------------------------------------------------------------------
 */
static int
ScanCloseObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc,
                Tcl_Obj *const *objv)
{
    Tcl_Obj    *obj;
    Scan       *scan;
    Ns_ObjvSpec args[] = {
      {"handle", Ns_ObjvObj, &obj, NULL},
      {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    scan = GetScan(interp, obj);
    if (scan == NULL) {
        return TCL_ERROR;
    }
    ScanClose(scan);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ScanTriplesObjCmd --
 *
 *      This command implements "ns_accesslog scan triples". It resolves a
 *      shared batch and returns its requested triples view.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Allocates the bounded view and sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
ScanTriplesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc,
                  Tcl_Obj *const *objv)
{
    Tcl_Obj          *obj;
    ScanState        *state;
    const Ns_DbBatch *batch;
    TCL_SIZE_T        remain;
    Ns_ObjvSpec       args[] = {
      {"batch",   Ns_ObjvObj,  &obj, NULL},
      {"?option", Ns_ObjvArgs, &remain, NULL},
      {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    state = GetState(interp);
    if (state == NULL || state->batchApi->get(interp, obj, &batch) != TCL_OK) {
        return TCL_ERROR;
    }
    return Triples(interp, state->batchApi, batch, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * NsLogScanObjCmd --
 *
 *      This command implements "ns_accesslog scan". It dispatches the scan
 *      ensemble to its subcommands.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      May open, read or close a source and sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
int
NsLogScanObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const *objv,
                const NsLogScanDefaults *defaults)
{
    static const Ns_SubCmdSpec subcmds[] = {
        {"open", OpenScanObjCmnd},  {"next", ScanNextObjCmd},       {"info", ScanInfoObjCmd},
        {"close", ScanCloseObjCmd}, {"triples", ScanTriplesObjCmd}, {NULL, NULL}};

    return Ns_SubsubcmdObjv(subcmds, (ClientData)defaults, interp, 1, objc, objv);
}

/*
 *      Local Variables: mode: c c-basic-offset: 4 fill-column: 70
 *      indent-tabs-mode: nil End:
 */
