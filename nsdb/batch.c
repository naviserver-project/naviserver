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

static void     DictPut(Tcl_Obj *dict, NsAtomId key, Tcl_Obj *value) NS_GNUC_NONNULL(1, 3);
static void     StringPut(Tcl_Obj *dict, NsAtomId key, const char *value) NS_GNUC_NONNULL(1, 3);
static void     WidePut(Tcl_Obj *dict, NsAtomId key, Tcl_WideInt value) NS_GNUC_NONNULL(1);
static Tcl_Obj *GetKey(Tcl_Interp *interp, Tcl_Obj *dict, NsAtomId key) NS_GNUC_NONNULL(1, 2);
static bool     ValidDescriptor(Tcl_Obj *obj) NS_GNUC_NONNULL(1);
static void     SchemaRelease(Ns_DbBatchSchema *schema) NS_GNUC_NONNULL(1);

static Ns_DbBatchSchema *SchemaCreate(Tcl_Interp *interp, Tcl_Obj *document) NS_GNUC_NONNULL(1, 2);
static Tcl_Obj          *AdoptBatch(Tcl_Interp *interp, Ns_DbBatchSchema *schema, Tcl_Obj *metadata,
                                    size_t rows, void *payload, Ns_DbBatchReadProc *read,
                                    Ns_DbBatchFreeProc *release) NS_GNUC_NONNULL(1, 2, 3);

static int  GetBatch(Tcl_Interp *interp, Tcl_Obj *obj, const Ns_DbBatch **batch)
    NS_GNUC_NONNULL(1, 2, 3);
static Tcl_Obj *BatchInfo(const Ns_DbBatch *batch) NS_GNUC_NONNULL(1);
static int BatchTriples(Tcl_Interp *interp, const Ns_DbBatch *batch, size_t offset, size_t limit)
    NS_GNUC_NONNULL(1, 2);
static bool BatchCompatible(const Ns_DbBatchDriverOps *ops, const Ns_DbBatch *batch)
    NS_GNUC_NONNULL(1, 2);

static Tcl_FreeInternalRepProc BatchFree      NS_GNUC_NONNULL(1);
static Tcl_DupInternalRepProc BatchDup        NS_GNUC_NONNULL(1, 2);
static Tcl_UpdateStringProc BatchUpdateString NS_GNUC_NONNULL(1);
static Tcl_ObjType batchType = {"nsdbBatch", BatchFree, BatchDup, BatchUpdateString,
                                Ns_TclSetFromAnyError};

static TCL_OBJCMDPROC_T BatchInfoObjCmd       NS_GNUC_NONNULL(2, 4);
static TCL_OBJCMDPROC_T BatchInsertObjCmd     NS_GNUC_NONNULL(2, 4);

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchError --
 *
 *      Report a batch error with its structured Tcl error code.
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
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates the dictionary and retains the value.
 *
 *----------------------------------------------------------------------
 */
static void
DictPut(Tcl_Obj *d, NsAtomId k, Tcl_Obj *v)
{
    (void)Tcl_DictObjPut(NULL, d, NsAtomObj(k), v);
}

/*
 *----------------------------------------------------------------------
 *
 * StringPut --
 *
 *      Store a driver-supplied string under a core atom key.
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
StringPut(Tcl_Obj *d, NsAtomId k, const char *v)
{
    DictPut(d, k, Tcl_NewStringObj(v, TCL_INDEX_NONE));
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
WidePut(Tcl_Obj *d, NsAtomId k, Tcl_WideInt v)
{
    DictPut(d, k, Tcl_NewWideIntObj(v));
}

/*
 *----------------------------------------------------------------------
 *
 * GetKey --
 *
 *      Look up a schema field using a core atom key.
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
    Tcl_Obj *name = NsAtomObj(key), *v = NULL;

    Tcl_IncrRefCount(name);
    if (Tcl_DictObjGet(interp, dict, name, &v) != TCL_OK) {
        v = NULL;
    }
    Tcl_DecrRefCount(name);
    return v;
} /* C descriptor/identifier strings cannot represent embedded NUL. Values remain length-delimited and may contain NUL; this restriction is only for names. */

/*
 *----------------------------------------------------------------------
 *
 * ValidDescriptor --
 *
 *      Check that a nonempty descriptor can be represented as a C string.
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
ValidDescriptor(Tcl_Obj *obj)
{
    TCL_SIZE_T  length;
    const char *s = Tcl_GetStringFromObj(obj, &length);

    return length > 0 && memchr(s, 0, (size_t)length) == NULL && strstr(s, "\xc0\x80") == NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * SchemaRelease --
 *
 *      Release a schema reference and its owned data at the last reference.
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
SchemaRelease(Ns_DbBatchSchema *s)
{
    if (--s->refs == 0u) {
        for (size_t c = 0; c < s->columns; c++) {
            if (s->names[c] != NULL) {
                Tcl_DecrRefCount(s->names[c]);
            }
            ns_free((void *)s->fields[c].name);
            ns_free((void *)s->fields[c].type);
            ns_free((void *)s->fields[c].refinement);
            ns_free((void *)s->fields[c].representation);
        }
        Tcl_DecrRefCount(s->document);
        Tcl_DecrRefCount(s->nullValue);
        for (size_t i = 0; i < 6u; i++) {
            Tcl_DecrRefCount(s->types[i]);
        }
        Tcl_FreeEncoding(s->utf8);
        ns_free(s->names);
        ns_free(s->fields);
        ns_free(s);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SchemaCreate --
 *
 *      Validate a flat scalar schema and retain its independent descriptors.
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
SchemaCreate(Tcl_Interp *interp, Tcl_Obj *document)
{
    Tcl_Obj   *columns = GetKey(interp, document, NS_ATOM_columns), **v, **root, **atoms, *args[5];
    TCL_SIZE_T n, count;
    Tcl_Obj   *id = GetKey(interp, document, NS_ATOM_id),
        *version = GetKey(interp, document, NS_ATOM_version);
    int               schemaVersion;
    Ns_DbBatchSchema *s;
    int               rc;

    if (id == NULL || *Tcl_GetString(id) == '\0' || version == NULL
        || Tcl_GetIntFromObj(interp, version, &schemaVersion) != TCL_OK || schemaVersion < 1
        || columns == NULL || Tcl_ListObjGetElements(interp, columns, &n, &v) != TCL_OK || n < 1
        || n > 4096) {
        (void)Ns_DbBatchError(interp, "TYPE_MISMATCH", "expected a nonempty flat batch schema");
        return NULL;
    } /* Obtain the core's shared JSON atoms and dedicated null through ns_json. */
    args[0] = NsAtomObj(NS_ATOM_ns_json);
    args[1] = NsAtomObj(NS_ATOM_parse);
    args[2] = NsAtomObj(NS_ATOM_DASH_output);
    args[3] = NsAtomObj(NS_ATOM_triples);
    args[4] = Tcl_NewStringObj("{\"s\":\"\",\"n\":0,\"b\":true,\"z\":null,\"o\":{},\"a\":[]}", -1);
    for (size_t i = 0; i < 5u; i++) {
        Tcl_IncrRefCount(args[i]);
    }
    rc = Tcl_EvalObjv(interp, 5, args, TCL_EVAL_GLOBAL);
    for (size_t i = 0; i < 5u; i++) {
        Tcl_DecrRefCount(args[i]);
    }
    if (rc != TCL_OK) {
        return NULL;
    }
    if (Tcl_ListObjGetElements(interp, Tcl_GetObjResult(interp), &count, &root) != TCL_OK
        || count != 3 || Tcl_ListObjGetElements(interp, root[2], &count, &atoms) != TCL_OK
        || count != 18) {
        return NULL;
    }
    s = ns_calloc(1u, sizeof(*s));
    s->refs = 1u;
    s->owner = interp;
    s->document = document;
    Tcl_IncrRefCount(document);
    s->utf8 = Tcl_GetEncoding(interp, "utf-8");
    s->columns = (size_t)n;
    s->fields = ns_calloc(s->columns, sizeof(*s->fields));
    s->names = ns_calloc(s->columns, sizeof(*s->names));
    for (size_t i = 0; i < 6u; i++) {
        s->types[i] = atoms[i * 3u + 1u];
        Tcl_IncrRefCount(s->types[i]);
    }
    s->nullValue = atoms[11];
    Tcl_IncrRefCount(
                     s->nullValue); /* Tcl evaluation and later caller-side shimmering may replace internal * list/dict reps. Keep our own names and C descriptor strings, never borrowed * pointers into children owned only by the schema's current Tcl rep. */
    if (Tcl_ListObjGetElements(interp, columns, &n, &v) != TCL_OK || (size_t)n != s->columns) {
        goto invalid;
    }
    for (size_t i = 0; i < s->columns; i++) {
        Tcl_Obj *name = GetKey(interp, v[i], NS_ATOM_name),
            *type = GetKey(interp, v[i], NS_ATOM_type),
            *ref = GetKey(interp, v[i], NS_ATOM_refinement),
            *rep = GetKey(interp, v[i], NS_ATOM_representation),
            *nullable = GetKey(interp, v[i], NS_ATOM_nullable);
        int null = 0;

        if (name == NULL || type == NULL || !ValidDescriptor(name) || !ValidDescriptor(type)
            || (ref != NULL && !ValidDescriptor(ref)) || (rep != NULL && !ValidDescriptor(rep))
            || (nullable != NULL && Tcl_GetBooleanFromObj(interp, nullable, &null) != TCL_OK)) {
            goto invalid;
        }
        s->names[i] = name;
        Tcl_IncrRefCount(name);
        s->fields[i].name = ns_strdup(Tcl_GetString(name));
        s->fields[i].type = ns_strdup(Tcl_GetString(type));
        s->fields[i].refinement = ref == NULL ? NULL : ns_strdup(Tcl_GetString(ref));
        s->fields[i].representation = rep == NULL ? NULL : ns_strdup(Tcl_GetString(rep));
        s->fields[i].nullable = null != 0;
        if (*s->fields[i].name == '\0'
            || (strcmp(s->fields[i].type, "string") != 0 && strcmp(s->fields[i].type, "number") != 0
                && strcmp(s->fields[i].type, "boolean") != 0
                && strcmp(s->fields[i].type, "null") != 0)) {
            goto invalid;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(s->fields[j].name, s->fields[i].name) == 0) {
                goto invalid;
            }
        }
    }
    Tcl_ResetResult(interp);
    return s;
 invalid:
    SchemaRelease(s);
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
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments the batch reference count.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DbBatchRetain(Ns_DbBatch *b)
{
    b->refs++;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchRelease --
 *
 *      Release a batch reference and destroy it at the last reference.
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
Ns_DbBatchRelease(Ns_DbBatch *b)
{
    if (--b->refs == 0u) {
        b->release(b->payload);
        SchemaRelease(b->schema);
        Tcl_DecrRefCount(b->metadata);
        ns_free(b);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * BatchFree --
 *
 *      Release the batch owned by a Tcl object internal representation.
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
BatchFree(Tcl_Obj *o)
{
    Ns_DbBatchRelease(o->internalRep.twoPtrValue.ptr1);
}

/*
 *----------------------------------------------------------------------
 *
 * BatchDup --
 *
 *      Duplicate a batch object internal representation without copying rows.
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
BatchDup(Tcl_Obj *src, Tcl_Obj *dst)
{
    Ns_DbBatch *b = src->internalRep.twoPtrValue.ptr1;

    b->refs++;
    dst->internalRep.twoPtrValue.ptr1 = b;
    dst->internalRep.twoPtrValue.ptr2 = NULL;
    dst->typePtr = &batchType;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchUpdateString --
 *
 *      Generate the descriptive string label of an opaque batch object.
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
BatchUpdateString(Tcl_Obj *o)
{
    static const char label[] = "nsdb-batch";

    o->bytes = Tcl_Alloc(sizeof(label));
    memcpy(o->bytes, label, sizeof(label));
    o->length = sizeof(label) - 1u;
}

/*
 *----------------------------------------------------------------------
 *
 * AdoptBatch --
 *
 *      Wrap producer-owned lexical storage in an immutable shared batch.
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
AdoptBatch(Tcl_Interp *interp, Ns_DbBatchSchema *schema, Tcl_Obj *metadata, size_t rows,
           void *payload, Ns_DbBatchReadProc *read, Ns_DbBatchFreeProc *release)
{
    Ns_DbBatch *b;
    Tcl_Obj    *o;
    TCL_SIZE_T  size;

    if (schema->owner != interp || read == NULL || release == NULL
        || Tcl_DictObjSize(interp, metadata, &size) != TCL_OK) {
        return NULL;
    }
    b = ns_calloc(1u, sizeof(*b));
    b->refs = 1u;
    b->schema = schema;
    schema->refs++;
    b->rows = rows;
    b->payload = payload;
    b->read = read;
    b->release = release;
    b->metadata = Tcl_DuplicateObj(metadata);
    Tcl_IncrRefCount(b->metadata);
    WidePut(b->metadata, NS_ATOM_rows, (Tcl_WideInt)rows);
    DictPut(b->metadata, NS_ATOM_schema, schema->document);
    o = Tcl_NewObj();
    Tcl_InvalidateStringRep(o);
    o->typePtr = &batchType;
    o->internalRep.twoPtrValue.ptr1 = b;
    o->internalRep.twoPtrValue.ptr2 = NULL;
    return o;
}

/*
 *----------------------------------------------------------------------
 *
 * GetBatch --
 *
 *      Resolve an opaque batch belonging to the current interpreter.
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
GetBatch(Tcl_Interp *interp, Tcl_Obj *obj, const Ns_DbBatch **b)
{
    if (obj->typePtr != &batchType) {
        return Ns_DbBatchError(interp, "TYPE_MISMATCH",
                               "expected an opaque nsdb batch, not its string label");
    }
    *b = obj->internalRep.twoPtrValue.ptr1;
    if ((*b)->schema->owner != interp) {
        return Ns_DbBatchError(interp, "TYPE_MISMATCH", "batch belongs to another interpreter");
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchInfo --
 *
 *      Return the immutable metadata associated with a batch.
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
BatchInfo(const Ns_DbBatch *b)
{
    return b->metadata;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchRowCount --
 *
 *      Return the number of rows in a batch.
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
Ns_DbBatchRowCount(const Ns_DbBatch *b)
{
    return b->rows;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchColumnCount --
 *
 *      Return the number of schema columns in a batch.
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
Ns_DbBatchColumnCount(const Ns_DbBatch *b)
{
    return b->schema->columns;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchGetColumn --
 *
 *      Return a column descriptor by zero-based index.
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
Ns_DbBatchGetColumn(const Ns_DbBatch *b, size_t c)
{
    return c < b->schema->columns ? &b->schema->fields[c] : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBatchGetValue --
 *
 *      Read a lexical value through the producer callback.
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
Ns_DbBatchGetValue(const Ns_DbBatch *b, size_t r, size_t c, Ns_DbBatchValue *value)
{
    memset(value, 0, sizeof(*value));
    if (r >= b->rows || c >= b->schema->columns || b->read(b->payload, r, c, value) != NS_OK) {
        return NS_ERROR;
    }
    if (value->isNull) {
        value->bytes = NULL;
        value->length = 0u;
        return b->schema->fields[c].nullable || strcmp(b->schema->fields[c].type, "null") == 0
            ? NS_OK
            : NS_ERROR;
    }
    if (value->bytes == NULL || strcmp(b->schema->fields[c].type, "null") == 0) {
        return NS_ERROR;
    }
    if (strcmp(b->schema->fields[c].type, "number") == 0
        && !NsJsonNumberIsValid(value->bytes, value->length, NULL)) {
        return NS_ERROR;
    }
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchTriples --
 *
 *      Materialize a bounded batch view using ns_json triples and lexical
 *      values.
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
BatchTriples(Tcl_Interp *interp, const Ns_DbBatch *b, size_t offset, size_t limit)
{
    Ns_DbBatchSchema *s = b->schema;
    Tcl_Obj          *out = Tcl_NewListObj(0, NULL), *root[3];
    size_t            end = offset > b->rows ? b->rows : offset;

    end += limit > b->rows - end ? b->rows - end : limit;
    Tcl_IncrRefCount(out);
    for (size_t r = offset; r < end; r++) {
        Tcl_Obj *row = Tcl_NewListObj(0, NULL);

        Tcl_IncrRefCount(row);
        for (size_t c = 0; c < s->columns; c++) {
            Ns_DbBatchValue         v;
            Tcl_Obj                *value, *type;
            Tcl_DString             utf;
            const Ns_DbBatchColumn *field = &s->fields[c];

            if (Ns_DbBatchGetValue(b, r, c, &v) != NS_OK || v.length > INT_MAX) {
                Tcl_DecrRefCount(row);
                Tcl_DecrRefCount(out);
                return Ns_DbBatchError(interp, "CONVERSION", "invalid provider value");
            }
            type = s->types[v.isNull                             ? 3
                            : strcmp(field->type, "string") == 0 ? 0
                            : strcmp(field->type, "number") == 0 ? 1
                            : 2];
            if (v.isNull) {
                value = s->nullValue;
            } else {
                Tcl_ExternalToUtfDString(s->utf8, (const char *)v.bytes, (TCL_SIZE_T)v.length,
                                         &utf);
                value = Tcl_NewStringObj(utf.string, utf.length);
                Tcl_DStringFree(&utf);
            }
            Tcl_ListObjAppendElement(interp, row, s->names[c]);
            Tcl_ListObjAppendElement(interp, row, type);
            Tcl_ListObjAppendElement(interp, row, value);
        }
        Tcl_ListObjAppendElement(interp, out, Tcl_NewWideIntObj((Tcl_WideInt)r));
        Tcl_ListObjAppendElement(interp, out, s->types[4]);
        Tcl_ListObjAppendElement(interp, out, row);
        Tcl_DecrRefCount(row);
    }
    root[0] = Tcl_NewObj();
    root[1] = s->types[5];
    root[2] = out;
    Tcl_SetObjResult(interp, Tcl_NewListObj(3, root));
    Tcl_DecrRefCount(out);
    return TCL_OK;
}

static const Ns_DbBatchApi api = {
    NS_DB_BATCH_ABI, sizeof(Ns_DbBatchApi), SchemaCreate, SchemaRelease, AdoptBatch, GetBatch,
    BatchInfo,       BatchTriples};

/*
 *----------------------------------------------------------------------
 *
 * NsDbBatchInitInterp --
 *
 *      Publish the nsdb module C batch API in an interpreter.
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
 * BatchCompatible --
 *
 *      Check whether a sink supports every column in a batch schema.
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
BatchCompatible(const Ns_DbBatchDriverOps *ops, const Ns_DbBatch *b)
{
    for (size_t c = 0; c < Ns_DbBatchColumnCount(b); c++) {
        if (!ops->supports(Ns_DbBatchGetColumn(b, c))) {
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
    Ns_ObjvSpec options[] = {{"-handle", Ns_ObjvObj, &handleObj, NULL}, {NULL, NULL, NULL, NULL}};
    Ns_ObjvSpec optionalBatch[] = {{"?batch", Ns_ObjvObj, &batchObj, NULL},
                                   {NULL, NULL, NULL, NULL}};
    Ns_ObjvSpec requiredBatch[] = {{"batch", Ns_ObjvObj, &batchObj, NULL},
                                   {NULL, NULL, NULL, NULL}};

    if (objc > 2 && strcmp(Tcl_GetString(objv[2]), "-handle") == 0) {
        if (Ns_ParseObjv(options, optionalBatch, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }
    } else {
        if (Ns_ParseObjv(NULL, requiredBatch, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }
    }
    if (batchObj != NULL && GetBatch(interp, batchObj, &batch) != TCL_OK) {
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
    StringPut(result, NS_ATOM_driver, handle->driver);
    WidePut(result, NS_ATOM_supported, ops != NULL);
    WidePut(result, NS_ATOM_batchVersion, 1);
    if (ops != NULL) {
        StringPut(result, NS_ATOM_method, ops->method);
        DictPut(result, NS_ATOM_transaction, NsAtomObj(NS_ATOM_required));
    }
    if (batch != NULL) {
        WidePut(result, NS_ATOM_compatible, ops != NULL && BatchCompatible(ops, batch));
    }
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
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
    const Ns_DbBatch          *batch;
    Ns_DbHandle               *handle;
    const Ns_DbBatchDriverOps *ops;
    Tcl_Obj                   *handleObj, *table = NULL, *mapping = NULL, *batchObj, *result;
    Ns_DbBatchMapping         *map;
    size_t                     mapped = 0u;
    TCL_SIZE_T                 parts, remain;
    int                        rc = TCL_ERROR;
    Tcl_WideInt                inserted = 0;
    Ns_Time                    started;
    Ns_ObjvSpec                handleArgs[] = {
        {"handle", Ns_ObjvObj, &handleObj, NULL},
        {"?option", Ns_ObjvArgs, &remain, NULL},
        {NULL, NULL, NULL, NULL}};
    Ns_ObjvSpec                options[] = {
        {"-table", Ns_ObjvObj, &table, NULL},
        {"-columns", Ns_ObjvObj, &mapping, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"batch", Ns_ObjvObj, &batchObj, NULL}, {NULL, NULL, NULL, NULL}
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
    if (Ns_ParseObjv(options, args, interp, 3, objc, objv) != NS_OK
        || GetBatch(interp, batchObj, &batch) != TCL_OK) {
        return TCL_ERROR;
    }
    if (table == NULL || Tcl_ListObjLength(interp, table, &parts) != TCL_OK || parts < 1
        || parts > 2) {
        return Ns_DbBatchError(interp, "MAPPING",
                               "table must contain one or two identifier components");
    }
    for (TCL_SIZE_T i = 0; i < parts; i++) {
        Tcl_Obj *part;

        if (Tcl_ListObjIndex(interp, table, i, &part) != TCL_OK || !ValidDescriptor(part)) {
            return Ns_DbBatchError(interp, "MAPPING",
                                   "table components must be nonempty identifiers without NUL");
        }
    }
    if (!handle->connected || handle->fetchingRows || NsDbGetActive(handle)) {
        return Ns_DbBatchError(interp, "INSERT", "handle is disconnected or has an active result");
    }
    map = ns_calloc(Ns_DbBatchColumnCount(batch), sizeof(*map));
    if (mapping == NULL) {
        for (size_t c = 0; c < Ns_DbBatchColumnCount(batch); c++) {
            map[mapped].source = c;
            map[mapped++].destination = Ns_DbBatchGetColumn(batch, c)->name;
        }
    } else {
        Tcl_DictSearch search;
        Tcl_Obj       *key, *value;
        int            done;

        if (Tcl_DictObjFirst(interp, mapping, &search, &key, &value, &done) != TCL_OK) {
            goto finish;
        }
        while (!done) {
            size_t      c;
            const char *name = Tcl_GetString(key);

            if (!ValidDescriptor(key) || !ValidDescriptor(value)) {
                Tcl_DictObjDone(&search);
                Ns_DbBatchError(interp, "MAPPING",
                                "mapping names must be nonempty identifiers without NUL");
                goto finish;
            }
            for (c = 0; c < Ns_DbBatchColumnCount(batch); c++) {
                if (strcmp(name, Ns_DbBatchGetColumn(batch, c)->name) == 0) {
                    break;
                }
            }
            if (c == Ns_DbBatchColumnCount(batch) || mapped == Ns_DbBatchColumnCount(batch)) {
                Tcl_DictObjDone(&search);
                Ns_DbBatchError(interp, "MAPPING", "unknown source field");
                goto finish;
            }
            map[mapped].source = c;
            map[mapped++].destination = Tcl_GetString(value);
            Tcl_DictObjNext(&search, &key, &value, &done);
        }
        Tcl_DictObjDone(&search);
    }
    if (mapped == 0u) {
        Ns_DbBatchError(interp, "MAPPING", "empty column mapping");
        goto finish;
    }
    for (size_t c = 0; c < mapped; c++) {
        if (!ops->supports(Ns_DbBatchGetColumn(batch, map[c].source))) {
            Ns_DbBatchError(interp, "TYPE_MISMATCH",
                            "unsupported field type, refinement or representation");
            goto finish;
        }
        if (*map[c].destination == '\0') {
            Ns_DbBatchError(interp, "MAPPING", "empty destination column");
            goto finish;
        }
        for (size_t j = 0; j < c; j++) {
            if (strcasecmp(map[j].destination, map[c].destination) == 0) {
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
        WidePut(result, NS_ATOM_rows, inserted);
        StringPut(result, NS_ATOM_method, ops->method);
        Tcl_SetObjResult(interp, result);
    }
 finish:
    ns_free(map);
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
    return insert ? BatchInsertObjCmd(NULL, interp, objc, objv)
        : BatchInfoObjCmd(NULL, interp, objc, objv);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
