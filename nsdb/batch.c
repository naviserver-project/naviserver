/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * batch.c --
 *
 *      Shared immutable batches with schema-associated lexical values.
 *      Provides the C producer interface, on-demand JSON triples, and
 *      the ns_db batchinfo and batchinsert commands. Database drivers
 *      perform value conversion and native insertion at the sink.
 */

#include "db.h"
#include "../nsd/nsd.h"
#include "../nsd/nsatoms.h"

struct Ns_DbBatchSchema {
    unsigned          refs;
    Tcl_Interp       *owner;
    Tcl_Obj          *document, *nullValue, *types[6], **names;
    Tcl_Encoding      utf8;
    size_t            columns;
    Ns_DbBatchColumn *fields;
};

struct Ns_DbBatch {
    unsigned            refs;
    Ns_DbBatchSchema   *schema;
    Tcl_Obj            *metadata;
    size_t              rows;
    void               *payload;
    Ns_DbBatchReadProc *read;
    Ns_DbBatchFreeProc *release;
};

/*
 * A Tcl-row batch borrows ASCII lexical values for this synchronous call.
 * Only non-ASCII strings need an external UTF-8 copy. The owning row list is
 * retained until the driver returns; no application-specific producer exists.
 */
typedef struct {
    Ns_DbBatchValue value;
    size_t          offset;
    bool            converted;
} TclRowCell;

typedef struct {
    Tcl_Obj     *owner;
    size_t       columns;
    TclRowCell  *cells;
    Tcl_DString  utf8;
} TclRowPayload;

/*
 * Function prototypes
 */

static void DictPut(Tcl_Obj *dict, NsAtomId key, Tcl_Obj *value)
    NS_GNUC_NONNULL(1,3);
static void DictPutString(Tcl_Obj *dict, NsAtomId key, const char *value)
    NS_GNUC_NONNULL(1,3);
static void DictPutWide(Tcl_Obj *dict, NsAtomId key, Tcl_WideInt value)
    NS_GNUC_NONNULL(1);
static Tcl_Obj *DictGet(Tcl_Obj *dict, Tcl_Interp *interp, NsAtomId key)
    NS_GNUC_NONNULL(1,2);
static bool DescriptorIsValid(Tcl_Obj *obj)
    NS_GNUC_NONNULL(1);
static void BatchSchemaRelease(Ns_DbBatchSchema *schema)
    NS_GNUC_NONNULL(1);
static Ns_DbBatchSchema *BatchSchemaCreate(Tcl_Interp *interp, Tcl_Obj *document)
    NS_GNUC_NONNULL(1,2);
static Tcl_Obj *BatchObjAdopt(Tcl_Interp *interp, Ns_DbBatchSchema *schema,
                             Tcl_Obj *metadata, size_t rows, void *payload,
                             Ns_DbBatchReadProc *read, Ns_DbBatchFreeProc *release)
    NS_GNUC_NONNULL(1,2,3);
static int BatchObjGet(Tcl_Interp *interp, Tcl_Obj *obj, const Ns_DbBatch **batch)
    NS_GNUC_NONNULL(1,2,3);
static Tcl_Obj *BatchGetInfo(const Ns_DbBatch *batch)
    NS_GNUC_NONNULL(1);
static int BatchGetTriples(Tcl_Interp *interp, const Ns_DbBatch *batch,
                           size_t offset, size_t limit)
    NS_GNUC_NONNULL(1,2);
static bool BatchIsCompatible(const Ns_DbBatch *batch, const Ns_DbBatchDriverOps *ops)
    NS_GNUC_NONNULL(1,2);
static int TclRowsPrepare(Tcl_Obj *rowsObj, Tcl_Interp *interp, Tcl_Obj *document,
                          Tcl_Obj *nullsObj, Ns_DbBatch *batch)
    NS_GNUC_NONNULL(1,2,3,5);

static Ns_DbBatchReadProc TclRowsRead NS_GNUC_NONNULL(1,4);
static Ns_DbBatchFreeProc TclRowsFree NS_GNUC_NONNULL(1);
static Tcl_FreeInternalRepProc BatchObjFree NS_GNUC_NONNULL(1);
static Tcl_DupInternalRepProc BatchObjDup NS_GNUC_NONNULL(1,2);
static Tcl_UpdateStringProc BatchObjUpdateString NS_GNUC_NONNULL(1);
static TCL_OBJCMDPROC_T BatchInfoObjCmd NS_GNUC_NONNULL(2,4);
static TCL_OBJCMDPROC_T BatchInsertObjCmd NS_GNUC_NONNULL(2,4);

static Tcl_ObjType batchType = {
    "nsdb:batch",
    BatchObjFree,
    BatchObjDup,
    BatchObjUpdateString,
    Ns_TclSetFromAnyError
#ifdef TCL_OBJTYPE_V0
   ,TCL_OBJTYPE_V0
#endif
};

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchError --
 *
 *      Report a batch error with its structured Tcl error code.
 *
 *      Use the NS_DB BATCH error-code prefix so callers can distinguish
 *      validation and driver failures without parsing the message.
 *
 * Results:
 *      TCL_ERROR.
 *
 * Side effects:
 *      Replaces the interpreter result and error code.
 *
 *----------------------------------------------------------------------
 */
int
Ns_DbBatchError(Tcl_Interp *interp, const char *code, const char *message)
{
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message, TCL_INDEX_NONE));
    Tcl_SetErrorCode(interp, "NS_DB", "BATCH", code, NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * DictPut --
 *
 *      Store a value under a core atom key in a dictionary.
 *
 *      Use the shared core atom as the key. The caller must supply a writable
 *      dictionary; this helper does not evaluate Tcl scripts.
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
 * DictPutString --
 *
 *      Store a driver-supplied string under a core atom key.
 *
 *      Copy the supplied C string into a Tcl value. The dictionary retains
 *      that value after insertion.
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
DictPutString(Tcl_Obj *dict, NsAtomId key, const char *value)
{
    DictPut(dict, key, Tcl_NewStringObj(value, TCL_INDEX_NONE));
}

/*
 *----------------------------------------------------------------------
 *
 * DictPutWide --
 *
 *      Store a wide integer under a core atom key.
 *
 *      Keep counts as Tcl wide integers instead of formatting them through a
 *      temporary string.
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
DictPutWide(Tcl_Obj *dict, NsAtomId key, Tcl_WideInt value)
{
    DictPut(dict, key, Tcl_NewWideIntObj(value));
}

/*
 *----------------------------------------------------------------------
 *
 * DictGet --
 *
 *      Look up a schema field using a core atom key.
 *
 *      The dictionary owns the returned value. Retain the atom key while Tcl
 *      examines the dictionary representation.
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
DictGet(Tcl_Obj *dict, Tcl_Interp *interp, NsAtomId key)
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
 * DescriptorIsValid --
 *
 *      Check that a nonempty descriptor can be represented as a C string.
 *
 *      Column names, types and identifier components are passed to drivers as
 *      C strings. Reject both literal and Tcl modified UTF-8 NUL encodings;
 *      row values remain length-delimited.
 *
 * Results:
 *      True for a nonempty string without an embedded NUL.
 *
 * Side effects:
 *      May generate the object string representation.
 *
 *----------------------------------------------------------------------
 */
static bool
DescriptorIsValid(Tcl_Obj *obj)
{
    TCL_SIZE_T  length;
    const char *string = Tcl_GetStringFromObj(obj, &length);

    return length > 0 && memchr(string, 0, (size_t)length) == NULL && strstr(string,
                                "\xc0\x80") == NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchSchemaRelease --
 *
 *      Release a schema reference and its owned data at the last reference.
 *
 *      The schema owns its document, JSON type/null objects, encoding handle,
 *      field names and copied C descriptors. Release these only when the final
 *      batch or producer reference is dropped.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May free schema storage and release retained Tcl objects.
 *
 *----------------------------------------------------------------------
 */
static void
BatchSchemaRelease(Ns_DbBatchSchema *schema)
{
    if (--schema->refs == 0u) {
        for (size_t columnIndex = 0; columnIndex < schema->columns; columnIndex++) {
            if (schema->names[columnIndex] != NULL) {
                Tcl_DecrRefCount(schema->names[columnIndex]);
            }
            ns_free((void *)schema->fields[columnIndex].name);
            ns_free((void *)schema->fields[columnIndex].type);
            ns_free((void *)schema->fields[columnIndex].refinement);
            ns_free((void *)schema->fields[columnIndex].representation);
        }
        Tcl_DecrRefCount(schema->document);
        Tcl_DecrRefCount(schema->nullValue);
        for (size_t index = 0; index < 6u; index++) {
            Tcl_DecrRefCount(schema->types[index]);
        }
        Tcl_FreeEncoding(schema->utf8);
        ns_free(schema->names);
        ns_free(schema->fields);
        ns_free(schema);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * BatchSchemaCreate --
 *
 *      Validate a flat scalar schema and retain its independent descriptors.
 *
 *      Require a positive version and one to 4096 uniquely named scalar
 *      columns. Copy C descriptors and retain field-name objects independently
 *      of later shimmering of the document. Obtain the shared JSON type and
 *      null objects through ns_json; nested JSON fields are not accepted.
 *
 * Results:
 *      Owned schema reference, or NULL on error.
 *
 * Side effects:
 *      Uses ns_json type objects and may set the interpreter error result.
 *
 *----------------------------------------------------------------------
 */
static Ns_DbBatchSchema *
BatchSchemaCreate(Tcl_Interp *interp, Tcl_Obj *document)
{
    Tcl_Obj          *columns = DictGet(document, interp, NS_ATOM_columns);
    Tcl_Obj         **columnObjs, **root, **atoms, *args[5];
    TCL_SIZE_T        columnCount, count;
    Tcl_Obj          *id = DictGet(document, interp, NS_ATOM_id);
    Tcl_Obj          *version = DictGet(document, interp, NS_ATOM_version);
    int               schemaVersion;
    Ns_DbBatchSchema *schema;
    int               rc;

    if (id == NULL || *Tcl_GetString(id) == '\0' || version == NULL
        || Tcl_GetIntFromObj(interp, version, &schemaVersion) != TCL_OK || schemaVersion < 1
        || columns == NULL
        || Tcl_ListObjGetElements(interp, columns, &columnCount, &columnObjs) != TCL_OK
        || columnCount < 1
        || columnCount > 4096) {
        (void)Ns_DbBatchError(interp, "TYPE_MISMATCH", "expected a nonempty flat batch schema");
        return NULL;
    }

    /*
     * Obtain the core's shared JSON atoms and dedicated null through ns_json.
     */
    args[0] = NsAtomObj(NS_ATOM_ns_json);
    args[1] = NsAtomObj(NS_ATOM_parse);
    args[2] = NsAtomObj(NS_ATOM_DASH_output);
    args[3] = NsAtomObj(NS_ATOM_triples);
    args[4] = Tcl_NewStringObj("{\"s\":\"\",\"n\":0,\"b\":true,\"z\":null,\"o\":{},\"a\":[]}", -1);
    for (size_t index = 0; index < 5u; index++) {
        Tcl_IncrRefCount(args[index]);
    }
    rc = Tcl_EvalObjv(interp, 5, args, TCL_EVAL_GLOBAL);
    for (size_t index = 0; index < 5u; index++) {
        Tcl_DecrRefCount(args[index]);
    }
    if (rc != TCL_OK) {
        return NULL;
    }
    if (Tcl_ListObjGetElements(interp, Tcl_GetObjResult(interp), &count, &root) != TCL_OK
        || count != 3 || Tcl_ListObjGetElements(interp, root[2], &count, &atoms) != TCL_OK
        || count != 18) {
        return NULL;
    }
    schema = ns_calloc(1u, sizeof(*schema));
    schema->refs = 1u;
    schema->owner = interp;
    schema->document = document;
    Tcl_IncrRefCount(document);
    schema->utf8 = Tcl_GetEncoding(interp, "utf-8");
    schema->columns = (size_t)columnCount;
    schema->fields = ns_calloc(schema->columns, sizeof(*schema->fields));
    schema->names = ns_calloc(schema->columns, sizeof(*schema->names));
    for (size_t index = 0; index < 6u; index++) {
        schema->types[index] = atoms[index * 3u + 1u];
        Tcl_IncrRefCount(schema->types[index]);
    }
    schema->nullValue = atoms[11];
    Tcl_IncrRefCount(schema->nullValue);

    /*
     * Tcl evaluation and later caller-side shimmering may replace internal
     * list/dict representations. Retain our own names and C descriptors,
     * rather than borrowing children of the schema's current representation.
     */
    if (Tcl_ListObjGetElements(interp, columns, &columnCount, &columnObjs) != TCL_OK
        || (size_t)columnCount != schema->columns) {
        goto invalid;
    }
    for (size_t index = 0; index < schema->columns; index++) {
        Tcl_Obj *name = DictGet(columnObjs[index], interp, NS_ATOM_name);
        Tcl_Obj *type = DictGet(columnObjs[index], interp, NS_ATOM_type);
        Tcl_Obj *ref = DictGet(columnObjs[index], interp, NS_ATOM_refinement);
        Tcl_Obj *rep = DictGet(columnObjs[index], interp, NS_ATOM_representation);
        Tcl_Obj *nullable = DictGet(columnObjs[index], interp, NS_ATOM_nullable);
        int      null = 0;

        if (name == NULL || type == NULL || !DescriptorIsValid(name) || !DescriptorIsValid(type)
            || (ref != NULL && !DescriptorIsValid(ref)) || (rep != NULL && !DescriptorIsValid(rep))
            || (nullable != NULL && Tcl_GetBooleanFromObj(interp, nullable, &null) != TCL_OK)) {
            goto invalid;
        }
        schema->names[index] = name;
        Tcl_IncrRefCount(name);
        schema->fields[index].name = ns_strdup(Tcl_GetString(name));
        schema->fields[index].type = ns_strdup(Tcl_GetString(type));
        schema->fields[index].refinement = ref == NULL ? NULL : ns_strdup(Tcl_GetString(ref));
        schema->fields[index].representation = rep == NULL ? NULL : ns_strdup(Tcl_GetString(rep));
        schema->fields[index].nullable = null != 0;
        if (*schema->fields[index].name == '\0'
            || (strcmp(schema->fields[index].type, "string") != 0
                && strcmp(schema->fields[index].type, "number") != 0
                && strcmp(schema->fields[index].type, "boolean") != 0
                && strcmp(schema->fields[index].type, "null") != 0)) {
            goto invalid;
        }
        for (size_t previousIndex = 0; previousIndex < index; previousIndex++) {
            if (strcmp(schema->fields[previousIndex].name, schema->fields[index].name) == 0) {
                goto invalid;
            }
        }
    }
    Tcl_ResetResult(interp);
    return schema;

 invalid:
    BatchSchemaRelease(schema);
    (void)Ns_DbBatchError(interp, "TYPE_MISMATCH",
                          "invalid, duplicate or unsupported batch schema field");
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchRetain --
 *
 *      Retain an immutable batch.
 *
 *      The producer, Tcl objects and synchronous consumers share the immutable
 *      payload through explicit references. Retaining does not copy values or
 *      serialize rows.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments the batch reference count.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DbBatchRetain(Ns_DbBatch *batch)
{
    batch->refs++;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchRelease --
 *
 *      Release a batch reference and destroy it at the last reference.
 *
 *      The last owner invokes the producer release callback exactly once, then
 *      releases the schema and metadata. The caller must hold a valid batch
 *      reference.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May invoke the producer release callback and free batch storage.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DbBatchRelease(Ns_DbBatch *batch)
{
    if (--batch->refs == 0u) {
        batch->release(batch->payload);
        BatchSchemaRelease(batch->schema);
        Tcl_DecrRefCount(batch->metadata);
        ns_free(batch);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * BatchObjFree --
 *
 *      Release the batch owned by a Tcl object internal representation.
 *
 *      This Tcl internal-representation callback drops one reference. Other
 *      Tcl objects and producer references may keep the batch alive.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Drops the retained batch reference.
 *
 *----------------------------------------------------------------------
 */
static void
BatchObjFree(Tcl_Obj *obj)
{
    Ns_DbBatchRelease(obj->internalRep.twoPtrValue.ptr1);
}

/*
 *----------------------------------------------------------------------
 *
 * BatchObjDup --
 *
 *      Duplicate a batch object internal representation without copying rows.
 *
 *      The source and destination share the same immutable payload and schema.
 *      Tcl manages their string representations separately.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Retains the batch and initializes the destination representation.
 *
 *----------------------------------------------------------------------
 */
static void
BatchObjDup(Tcl_Obj *src, Tcl_Obj *dst)
{
    Ns_DbBatch *batch = src->internalRep.twoPtrValue.ptr1;

    batch->refs++;
    dst->internalRep.twoPtrValue.ptr1 = batch;
    dst->internalRep.twoPtrValue.ptr2 = NULL;
    dst->typePtr = &batchType;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchObjUpdateString --
 *
 *      Generate the descriptive string label of an opaque batch object.
 *
 *      The label identifies the object type for display only. It contains no
 *      row data and cannot reconstruct a batch after its internal
 *      representation is lost.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets the object string representation; no rows are serialized.
 *
 *----------------------------------------------------------------------
 */
static void
BatchObjUpdateString(Tcl_Obj *obj)
{
    static const char label[] = "nsdb-batch";

    obj->bytes = Tcl_Alloc(sizeof(label));
    memcpy(obj->bytes, label, sizeof(label));
    obj->length = sizeof(label) - 1u;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchObjAdopt --
 *
 *      Wrap producer-owned lexical storage in an immutable shared batch.
 *
 *      Accept only a schema owned by the supplied interpreter, valid
 *      reader/release callbacks and dictionary metadata. Duplicate metadata,
 *      retain the schema and publish an opaque Tcl object; on validation
 *      failure the producer still owns the payload.
 *
 * Results:
 *      New Tcl object, or NULL on error; payload is adopted only on success.
 *
 * Side effects:
 *      Retains the schema and a metadata copy without copying row storage.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
BatchObjAdopt(Tcl_Interp *interp, Ns_DbBatchSchema *schema, Tcl_Obj *metadata, size_t rows,
              void *payload, Ns_DbBatchReadProc *read, Ns_DbBatchFreeProc *release)
{
    Ns_DbBatch *batch;
    Tcl_Obj    *obj;
    TCL_SIZE_T  size;

    if (schema->owner != interp || read == NULL || release == NULL
        || Tcl_DictObjSize(interp, metadata, &size) != TCL_OK) {
        return NULL;
    }
    batch = ns_calloc(1u, sizeof(*batch));
    batch->refs = 1u;
    batch->schema = schema;
    schema->refs++;
    batch->rows = rows;
    batch->payload = payload;
    batch->read = read;
    batch->release = release;
    batch->metadata = Tcl_DuplicateObj(metadata);
    Tcl_IncrRefCount(batch->metadata);
    DictPutWide(batch->metadata, NS_ATOM_rows, (Tcl_WideInt)rows);
    DictPut(batch->metadata, NS_ATOM_schema, schema->document);
    obj = Tcl_NewObj();
    Tcl_InvalidateStringRep(obj);
    obj->typePtr = &batchType;
    obj->internalRep.twoPtrValue.ptr1 = batch;
    obj->internalRep.twoPtrValue.ptr2 = NULL;
    return obj;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchObjGet --
 *
 *      Resolve an opaque batch belonging to the current interpreter.
 *
 *      A string label is insufficient: require the batch Tcl_Obj type and the
 *      schema owner interpreter. The returned batch is borrowed from the
 *      object and must not be released by the caller.
 *
 * Results:
 *      TCL_OK with a borrowed batch, or TCL_ERROR.
 *
 * Side effects:
 *      May set the interpreter error result.
 *
 *----------------------------------------------------------------------
 */
static int
BatchObjGet(Tcl_Interp *interp, Tcl_Obj *obj, const Ns_DbBatch **batch)
{
    if (obj->typePtr != &batchType) {
        return Ns_DbBatchError(interp, "TYPE_MISMATCH",
                               "expected an opaque nsdb batch, not its string label");
    }
    *batch = obj->internalRep.twoPtrValue.ptr1;
    if ((*batch)->schema->owner != interp) {
        return Ns_DbBatchError(interp, "TYPE_MISMATCH", "batch belongs to another interpreter");
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchGetInfo --
 *
 *      Return the immutable metadata associated with a batch.
 *
 *      The dictionary belongs to the immutable batch. Callers needing to
 *      modify it must first obtain a writable copy.
 *
 * Results:
 *      Borrowed metadata dictionary.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
BatchGetInfo(const Ns_DbBatch *batch)
{
    return batch->metadata;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchRowCount --
 *
 *      Return the number of rows in a batch.
 *
 *      This accessor does not decode values or invoke the producer callback.
 *
 * Results:
 *      Row count.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
size_t
Ns_DbBatchRowCount(const Ns_DbBatch *batch)
{
    return batch->rows;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchColumnCount --
 *
 *      Return the number of schema columns in a batch.
 *
 *      The column count belongs to the retained schema and is shared by every
 *      row.
 *
 * Results:
 *      Column count.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
size_t
Ns_DbBatchColumnCount(const Ns_DbBatch *batch)
{
    return batch->schema->columns;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchGetColumn --
 *
 *      Return a column descriptor by zero-based index.
 *
 *      The descriptor and its C strings remain owned by the schema. No
 *      allocation or conversion occurs.
 *
 * Results:
 *      Borrowed descriptor, or NULL when the index is out of range.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const Ns_DbBatchColumn *
Ns_DbBatchGetColumn(const Ns_DbBatch *batch, size_t columnIndex)
{
    return columnIndex < batch->schema->columns ? &batch->schema->fields[columnIndex] : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchGetValue --
 *
 *      Read a lexical value through the producer callback.
 *
 *      Clear the output before checking bounds and invoking the producer.
 *      Normalize NULL cells, enforce schema nullability and validate lexical
 *      JSON numbers. Successful non-NULL bytes are borrowed from the retained
 *      producer payload.
 *
 * Results:
 *      NS_OK with a borrowed value, or NS_ERROR for an invalid value or
 *      index.
 *
 * Side effects:
 *      Initializes the output descriptor and invokes the producer reader.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_DbBatchGetValue(const Ns_DbBatch *batch, size_t rowIndex, size_t columnIndex,
                   Ns_DbBatchValue *value)
{
    memset(value, 0, sizeof(*value));
    if (rowIndex >= batch->rows || columnIndex >= batch->schema->columns
        || batch->read(batch->payload, rowIndex, columnIndex, value) != NS_OK) {
        return NS_ERROR;
    }
    if (value->isNull) {
        value->bytes = NULL;
        value->length = 0u;
        return batch->schema->fields[columnIndex].nullable || strcmp(batch->schema->fields[columnIndex].type,
            "null") == 0
            ? NS_OK
            : NS_ERROR;
    }
    if (value->bytes == NULL || strcmp(batch->schema->fields[columnIndex].type, "null") == 0) {
        return NS_ERROR;
    }
    if (strcmp(batch->schema->fields[columnIndex].type, "number") == 0
        && !NsJsonNumberIsValid(value->bytes, value->length, NULL)) {
        return NS_ERROR;
    }
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchGetTriples --
 *
 *      Materialize a bounded batch view using ns_json triples and lexical
 *      values.
 *
 *      Clamp the requested range to the available rows without overflowing
 *      offset plus limit. Decode UTF-8 only for the requested cells and
 *      preserve JSON type/null objects from the schema. An invalid provider
 *      value aborts the view without returning partial rows.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Allocates the requested view and sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
BatchGetTriples(Tcl_Interp *interp, const Ns_DbBatch *batch, size_t offset, size_t limit)
{
    Ns_DbBatchSchema *schema = batch->schema;
    Tcl_Obj          *out = Tcl_NewListObj(0, NULL), *root[3];
    size_t            end = offset > batch->rows ? batch->rows : offset;

    end += limit > batch->rows - end ? batch->rows - end : limit;
    Tcl_IncrRefCount(out);
    for (size_t rowIndex = offset; rowIndex < end; rowIndex++) {
        Tcl_Obj *row = Tcl_NewListObj(0, NULL);

        Tcl_IncrRefCount(row);
        for (size_t columnIndex = 0; columnIndex < schema->columns; columnIndex++) {
            Ns_DbBatchValue         cellValue;
            Tcl_Obj                *value, *type;
            Tcl_DString             utf;
            const Ns_DbBatchColumn *field = &schema->fields[columnIndex];

            if (Ns_DbBatchGetValue(batch, rowIndex, columnIndex, &cellValue) != NS_OK
                || cellValue.length > INT_MAX) {
                Tcl_DecrRefCount(row);
                Tcl_DecrRefCount(out);
                return Ns_DbBatchError(interp, "CONVERSION", "invalid provider value");
            }
            type = schema->types[cellValue.isNull ? 3
                                 : strcmp(field->type, "string") == 0 ? 0
                                 : strcmp(field->type, "number") == 0 ? 1
                                 : 2];
            if (cellValue.isNull) {
                value = schema->nullValue;
            } else {
                Tcl_ExternalToUtfDString(schema->utf8, (const char *)cellValue.bytes,
                                         (TCL_SIZE_T)cellValue.length,
                                         &utf);
                value = Tcl_NewStringObj(utf.string, utf.length);
                Tcl_DStringFree(&utf);
            }
            Tcl_ListObjAppendElement(interp, row, schema->names[columnIndex]);
            Tcl_ListObjAppendElement(interp, row, type);
            Tcl_ListObjAppendElement(interp, row, value);
        }
        Tcl_ListObjAppendElement(interp, out, Tcl_NewWideIntObj((Tcl_WideInt)rowIndex));
        Tcl_ListObjAppendElement(interp, out, schema->types[4]);
        Tcl_ListObjAppendElement(interp, out, row);
        Tcl_DecrRefCount(row);
    }
    root[0] = Tcl_NewObj();
    root[1] = schema->types[5];
    root[2] = out;
    Tcl_SetObjResult(interp, Tcl_NewListObj(3, root));
    Tcl_DecrRefCount(out);
    return TCL_OK;
}

static const Ns_DbBatchApi api = {
    NS_DB_BATCH_ABI,
    sizeof(Ns_DbBatchApi),
    BatchSchemaCreate,
    BatchSchemaRelease,
    BatchObjAdopt,
    BatchObjGet,
    BatchGetInfo,
    BatchGetTriples
};

/*
 *----------------------------------------------------------------------
 *
 * NsDbBatchInitInterp --
 *
 *      Publish the nsdb module C batch API in an interpreter.
 *
 *      The associated API table has module lifetime and needs no interpreter
 *      cleanup callback. Producer modules use it without depending on private
 *      batch structures.
 *
 * Results:
 *      TCL_OK.
 *
 * Side effects:
 *      Installs immutable API data owned by the loaded nsdb module.
 *
 *----------------------------------------------------------------------
 */
int
NsDbBatchInitInterp(Tcl_Interp *interp)
{
    Tcl_SetAssocData(interp, NS_DB_BATCH_ASSOC_DATA, NULL, (ClientData)&api);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchIsCompatible --
 *
 *      Check whether a sink supports every column in a batch schema.
 *
 *      Ask the registered sink about every schema column, including refinement
 *      and representation. Stop at the first unsupported descriptor without
 *      reading row values.
 *
 * Results:
 *      True when all column descriptors are supported.
 *
 * Side effects:
 *      Invokes the driver capability callback for each column.
 *
 *----------------------------------------------------------------------
 */
static bool
BatchIsCompatible(const Ns_DbBatch *batch, const Ns_DbBatchDriverOps *ops)
{
    for (size_t columnIndex = 0; columnIndex < Ns_DbBatchColumnCount(batch); columnIndex++) {
        if (!ops->supports(Ns_DbBatchGetColumn(batch, columnIndex))) {
            return NS_FALSE;
        }
    }
    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchInfoObjCmd --
 *
 *      This command implements "ns_db batchinfo". It returns batch metadata
 *      or the selected database driver's batch capabilities.
 *
 *      With a batch, return its metadata. With -handle, report the registered
 *      driver capabilities and optionally check schema compatibility;
 *      capability queries do not require an active transaction.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
BatchInfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc,
                Tcl_Obj *const *objv)
{
    const Ns_DbBatch          *batch = NULL;
    Ns_DbHandle               *handle;
    const Ns_DbBatchDriverOps *ops;
    Tcl_Obj                   *batchObj = NULL, *handleObj = NULL, *result;
    Ns_ObjvSpec                options[] = {
        {"-handle", Ns_ObjvObj, &handleObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec                optionalBatch[] = {
        {"?batch", Ns_ObjvObj, &batchObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec                requiredBatch[] = {
        {"batch", Ns_ObjvObj, &batchObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (objc > 2 && strcmp(Tcl_GetString(objv[2]), "-handle") == 0) {
        if (Ns_ParseObjv(options, optionalBatch, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }
    } else {
        if (Ns_ParseObjv(NULL, requiredBatch, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }
    }
    if (batchObj != NULL && BatchObjGet(interp, batchObj, &batch) != TCL_OK) {
        return TCL_ERROR;
    }
    if (handleObj == NULL) {
        Tcl_SetObjResult(interp, batch->metadata);
        return TCL_OK;
    }
    if (Ns_TclDbGetHandle(interp, Tcl_GetString(handleObj), &handle) != TCL_OK) {
        return TCL_ERROR;
    }
    ops = NsDbGetBatchOps(handle);
    result = ops == NULL ? Tcl_NewDictObj() : ops->capabilities();
    DictPutString(result, NS_ATOM_driver, handle->driver);
    DictPutWide(result, NS_ATOM_supported, ops != NULL);
    DictPutWide(result, NS_ATOM_batchVersion, 1);
    Tcl_DictObjPut(interp, result, Tcl_NewStringObj("tclRows", TCL_INDEX_NONE),
                   Tcl_NewBooleanObj(ops != NULL));
    if (ops != NULL) {
        DictPutString(result, NS_ATOM_method, ops->method);
        DictPut(result, NS_ATOM_transaction, NsAtomObj(NS_ATOM_required));
    }
    if (batch != NULL) {
        DictPutWide(result, NS_ATOM_compatible, ops != NULL && BatchIsCompatible(batch, ops));
    }
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclRowsRead --
 *
 *      Return a borrowed lexical cell for a synchronous driver callback.
 *      Bounds and nullability are checked by Ns_DbBatchGetValue.
 *
 *      ASCII cells point into the retained Tcl row objects. Converted cells
 *      use offsets into the completed UTF-8 buffer so buffer growth during
 *      preparation cannot invalidate them.
 *
 * Results:
 *      NS_OK with a length-delimited cell; no allocation or Tcl evaluation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
TclRowsRead(const void *arg, size_t row, size_t column, Ns_DbBatchValue *value)
{
    const TclRowPayload *payload = arg;
    const TclRowCell    *cell = &payload->cells[row * payload->columns + column];

    *value = cell->value;
    if (cell->converted) {
        value->bytes = (const unsigned char *)payload->utf8.string + cell->offset;
    }
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclRowsFree --
 *
 *      Release call-local row ownership and converted UTF-8 storage.
 *
 *      Preparation and the sink share this call-local payload synchronously.
 *      It retains the outer row list and owns the cell descriptors and
 *      converted byte buffer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Decrements the input row object's reference count and frees storage.
 *
 *----------------------------------------------------------------------
 */
static void
TclRowsFree(void *arg)
{
    TclRowPayload *payload = arg;

    Tcl_DecrRefCount(payload->owner);
    Tcl_DStringFree(&payload->utf8);
    ns_free(payload->cells);
    ns_free(payload);
}

/*
 *----------------------------------------------------------------------
 *
 * TclRowsPrepare --
 *
 *      Validate rectangular Tcl rows and explicit per-row NULL column
 *      indices before invoking a driver. Empty strings are ordinary values.
 *      Numeric conversion remains the responsibility of the typed sink.
 *
 *      Retain the input rows for the synchronous sink call. Borrow ASCII
 *      strings directly and copy non-ASCII values into external UTF-8 storage.
 *      A null mask or null-typed column supplies explicit NULL; ordinary empty
 *      strings remain values. On success the caller owns the schema and
 *      payload through the output batch.
 *
 * Results:
 *      TCL_OK with a call-local batch, or TCL_ERROR without retained state.
 *
 * Side effects:
 *      Retains the input rows and allocates UTF-8 storage when required.
 *
 *----------------------------------------------------------------------
 */
static int
TclRowsPrepare(Tcl_Obj *rowsObj, Tcl_Interp *interp, Tcl_Obj *document,
               Tcl_Obj *nullsObj, Ns_DbBatch *batch)
{
    Ns_DbBatchSchema *schema = BatchSchemaCreate(interp, document);
    TclRowPayload    *payload;
    Tcl_Obj        **rows, **nullRows = NULL;
    TCL_SIZE_T       rowCount, nullCount;

    if (schema == NULL) {
        return TCL_ERROR;
    }
    if (Tcl_ListObjGetElements(interp, rowsObj, &rowCount, &rows) != TCL_OK
        || (nullsObj != NULL
            && (Tcl_ListObjGetElements(interp, nullsObj, &nullCount, &nullRows) != TCL_OK
                || nullCount != rowCount))) {
        BatchSchemaRelease(schema);
        return Ns_DbBatchError(interp, "ROWS", "NULL rows must match the number of prepared rows");
    }
    if ((size_t)rowCount > SIZE_MAX / schema->columns / sizeof(TclRowCell)) {
        BatchSchemaRelease(schema);
        return Ns_DbBatchError(interp, "ROWS", "prepared rows exceed allocation limits");
    }
    payload = ns_calloc(1u, sizeof(*payload));
    payload->owner = rowsObj;
    Tcl_IncrRefCount(rowsObj);
    payload->columns = schema->columns;
    payload->cells = ns_calloc(rowCount == 0 ? 1u : (size_t)rowCount * schema->columns,
                              sizeof(*payload->cells));
    Tcl_DStringInit(&payload->utf8);
    for (TCL_SIZE_T rowIndex = 0; rowIndex < rowCount; rowIndex++) {
        Tcl_Obj   **values;
        TCL_SIZE_T  count;

        if (Tcl_ListObjGetElements(interp, rows[rowIndex], &count, &values) != TCL_OK
            || (size_t)count != schema->columns) {
            Ns_DbBatchError(interp, "ROWS", "prepared row width does not match schema");
            goto invalid;
        }
        if (nullRows != NULL) {
            Tcl_Obj   **indices;
            TCL_SIZE_T  n;

            if (Tcl_ListObjGetElements(interp, nullRows[rowIndex], &n, &indices) != TCL_OK) {
                goto invalid;
            }
            for (TCL_SIZE_T index = 0; index < n; index++) {
                Tcl_WideInt column;

                if (Tcl_GetWideIntFromObj(interp, indices[index], &column) != TCL_OK
                    || column < 0 || (size_t)column >= schema->columns) {
                    Ns_DbBatchError(interp, "ROWS", "NULL column index is outside schema");
                    goto invalid;
                }
                payload->cells[(size_t)rowIndex * schema->columns + (size_t)column].value.isNull = true;
            }
        }
        for (size_t columnIndex = 0; columnIndex < schema->columns; columnIndex++) {
            TclRowCell *cell = &payload->cells[(size_t)rowIndex * schema->columns + columnIndex];
            TCL_SIZE_T  length;
            const char *text;

            if (strcmp(schema->fields[columnIndex].type, "null") == 0) {
                cell->value.isNull = true;
            } else if (cell->value.isNull && !schema->fields[columnIndex].nullable) {
                Ns_DbBatchError(interp, "TYPE_MISMATCH", "NULL supplied for a nonnullable field");
                goto invalid;
            }
            if (cell->value.isNull) {
                continue;
            }
            text = Tcl_GetStringFromObj(values[columnIndex], &length);
            cell->value.bytes = (const unsigned char *)text;
            cell->value.length = (size_t)length;
            for (TCL_SIZE_T index = 0; index < length; index++) {
                if ((unsigned char)text[index] >= 0x80u) {
                    Tcl_DString converted;

                    Tcl_DStringInit(&converted);
                    Tcl_UtfToExternalDString(schema->utf8, text, length, &converted);
                    cell->offset = (size_t)payload->utf8.length;
                    cell->value.length = (size_t)converted.length;
                    cell->converted = true;
                    Tcl_DStringAppend(&payload->utf8, converted.string, converted.length);
                    Tcl_DStringFree(&converted);
                    break;
                }
            }
        }
    }
    batch->schema = schema;
    batch->rows = (size_t)rowCount;
    batch->payload = payload;
    batch->read = TclRowsRead;
    batch->release = TclRowsFree;
    return TCL_OK;

 invalid:
    TclRowsFree(payload);
    BatchSchemaRelease(schema);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchInsertObjCmd --
 *
 *      This command implements "ns_db batchinsert". It validates the batch
 *      mapping and invokes the driver's native sink in the caller's
 *      transaction.
 *
 *      Validate identifiers, mapping, driver support and caller-owned
 *      transaction state before entering the sink. Accept either an opaque
 *      producer batch or prepared Tcl rows with -schema. Measure only the sink
 *      call with the existing SQL timer and leave transaction completion to
 *      the caller. Release call-local row storage on both success and failure.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Sets the interpreter result; the sink may write database rows.
 *
 *----------------------------------------------------------------------
 */
static int
BatchInsertObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc,
                  Tcl_Obj *const *objv)
{
    const Ns_DbBatch          *batch = NULL;
    Ns_DbHandle               *handle;
    const Ns_DbBatchDriverOps *ops;
    Tcl_Obj                   *handleObj, *table = NULL, *mapping = NULL, *batchObj, *result;
    Ns_DbBatchMapping         *map;
    Ns_DbBatch                 prepared = {0};
    Tcl_Obj                   *schema = NULL, *nulls = NULL;
    size_t                     mapped = 0u;
    TCL_SIZE_T                 parts, remain;
    int                        rc = TCL_ERROR;
    Tcl_WideInt                inserted = 0;
    Ns_Time                    started;
    Ns_ObjvSpec                handleArgs[] = {
        {"handle",  Ns_ObjvObj,  &handleObj, NULL},
        {"?option", Ns_ObjvArgs, &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec                options[] = {
        {"-table",   Ns_ObjvObj, &table,   NULL},
        {"-columns", Ns_ObjvObj, &mapping, NULL},
        {"-schema",  Ns_ObjvObj, &schema,  NULL},
        {"-nulls",   Ns_ObjvObj, &nulls,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec                args[] = {
        {"batch", Ns_ObjvObj, &batchObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, handleArgs, interp, 2, objc, objv) != NS_OK
        || Ns_TclDbGetHandle(interp, Tcl_GetString(handleObj), &handle) != TCL_OK) {
        /*
         *  Reject unsupported drivers before decoding batches or querying state.
         */
        return TCL_ERROR;
    }

    ops = NsDbGetBatchOps(handle);
    if (ops == NULL) {
        return Ns_DbBatchError(interp, "UNSUPPORTED",
                               "driver has no batch insertion implementation");
    }
    if (Ns_ParseObjv(options, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (table == NULL || Tcl_ListObjLength(interp, table, &parts) != TCL_OK || parts < 1
        || parts > 2) {
        return Ns_DbBatchError(interp, "MAPPING",
                               "table must contain one or two identifier components");
    }
    for (TCL_SIZE_T index = 0; index < parts; index++) {
        Tcl_Obj *part;

        if (Tcl_ListObjIndex(interp, table, index, &part) != TCL_OK || !DescriptorIsValid(part)) {
            return Ns_DbBatchError(interp, "MAPPING",
                                   "table components must be nonempty identifiers without NUL");
        }
    }
    if (!handle->connected || handle->fetchingRows || NsDbGetActive(handle)) {
        return Ns_DbBatchError(interp, "INSERT", "handle is disconnected or has an active result");
    }
    if (schema != NULL) {
        if (TclRowsPrepare(batchObj, interp, schema, nulls, &prepared) != TCL_OK) {
            return TCL_ERROR;
        }
        batch = &prepared;
    } else if (nulls != NULL) {
        return Ns_DbBatchError(interp, "ROWS", "-nulls requires -schema");
    } else if (BatchObjGet(interp, batchObj, &batch) != TCL_OK) {
        return TCL_ERROR;
    }
    map = ns_calloc(Ns_DbBatchColumnCount(batch), sizeof(*map));
    if (mapping == NULL) {
        for (size_t columnIndex = 0; columnIndex < Ns_DbBatchColumnCount(batch); columnIndex++) {
            map[mapped].source = columnIndex;
            map[mapped++].destination = Ns_DbBatchGetColumn(batch, columnIndex)->name;
        }
    } else {
        Tcl_DictSearch search;
        Tcl_Obj       *key, *value;
        int            done;

        if (Tcl_DictObjFirst(interp, mapping, &search, &key, &value, &done) != TCL_OK) {
            goto finish;
        }
        while (!done) {
            size_t      columnIndex;
            const char *name = Tcl_GetString(key);

            if (!DescriptorIsValid(key) || !DescriptorIsValid(value)) {
                Tcl_DictObjDone(&search);
                Ns_DbBatchError(interp, "MAPPING",
                                "mapping names must be nonempty identifiers without NUL");
                goto finish;
            }
            for (columnIndex = 0; columnIndex < Ns_DbBatchColumnCount(batch); columnIndex++) {
                if (strcmp(name, Ns_DbBatchGetColumn(batch, columnIndex)->name) == 0) {
                    break;
                }
            }
            if (columnIndex == Ns_DbBatchColumnCount(batch) || mapped == Ns_DbBatchColumnCount(batch)) {
                Tcl_DictObjDone(&search);
                Ns_DbBatchError(interp, "MAPPING", "unknown source field");
                goto finish;
            }
            map[mapped].source = columnIndex;
            map[mapped++].destination = Tcl_GetString(value);
            Tcl_DictObjNext(&search, &key, &value, &done);
        }
        Tcl_DictObjDone(&search);
    }
    if (mapped == 0u) {
        Ns_DbBatchError(interp, "MAPPING", "empty column mapping");
        goto finish;
    }
    for (size_t columnIndex = 0; columnIndex < mapped; columnIndex++) {
        if (!ops->supports(Ns_DbBatchGetColumn(batch, map[columnIndex].source))) {
            Ns_DbBatchError(interp, "TYPE_MISMATCH",
                            "unsupported field type, refinement or representation");
            goto finish;
        }
        if (*map[columnIndex].destination == '\0') {
            Ns_DbBatchError(interp, "MAPPING", "empty destination column");
            goto finish;
        }
        for (size_t previousIndex = 0; previousIndex < columnIndex; previousIndex++) {
            if (strcasecmp(map[previousIndex].destination, map[columnIndex].destination) == 0) {
                Ns_DbBatchError(interp, "MAPPING", "duplicate destination column");
                goto finish;
            }
        }
    }
    if (ops->transaction(handle) != NS_DB_BATCH_ACTIVE) {
        Ns_DbBatchError(interp, "TRANSACTION_REQUIRED",
                        "an active caller-owned transaction is required");
        goto finish;
    }
    Ns_GetTime(&started);
    rc = ops->insert(interp, handle, batch, table, map, mapped, &inserted);
    NsDbLogSql(&started, handle, "<batchinsert>");
    if (rc == TCL_OK) {
        result = Tcl_NewDictObj();
        DictPutWide(result, NS_ATOM_rows, inserted);
        DictPutString(result, NS_ATOM_method, ops->method);
        Tcl_SetObjResult(interp, result);
    }

 finish:
    ns_free(map);
    if (prepared.schema != NULL) {
        TclRowsFree(prepared.payload);
        BatchSchemaRelease(prepared.schema);
    }
    return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * NsDbBatchObjCmd --
 *
 *      This command implements "ns_db batchinfo" and "ns_db batchinsert" by
 *      selecting the corresponding command handler.
 *
 *      Keep the established command dispatch and callback signatures. Parsing,
 *      batch ownership checks and driver operations remain in the selected
 *      handler.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      See BatchInfoObjCmd and BatchInsertObjCmd.
 *
 *----------------------------------------------------------------------
 */
int
NsDbBatchObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const *objv, bool insert)
{
    return insert
        ? BatchInsertObjCmd(NULL, interp, objc, objv)
        : BatchInfoObjCmd(NULL, interp, objc, objv);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
