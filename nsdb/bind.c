/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * bind.c --
 *
 *      Transparent SQL parameter caching and typed binding for ordinary
 *      ns_db commands. Drivers execute the statement with separate values.
 */

#include "db.h"

typedef enum {
    BindScalar,
    BindList,
    BindRows,
    BindBinary
} BindValueType;

typedef struct BindSpec {
    BindValueType       kind;
    TCL_SIZE_T          columnCount;
    Ns_DbBindValueType *columns;
} BindSpec;

typedef struct BindOccurrence {
    TCL_SIZE_T offset, length, parameter;
} BindOccurrence;

/* Encoded buffers can outlive their cache slot during nested driver calls. */
typedef struct EncodedSql {
    unsigned      refs;          /* Cache and in-flight buffer owners. */
    Tcl_Encoding  encoding;      /* Retained system encoding handle. */
    Tcl_DString   bytes;
} EncodedSql;

typedef struct BindSql {
    unsigned                refs;         /* Tcl objects and active commands. */
    Tcl_Obj                *names;        /* Unique names, including prefixes. */
    BindOccurrence         *occurrences;
    TCL_SIZE_T              count;        /* Occurrences, not unique names. */
    size_t                  capacity;     /* Allocated occurrence slots. */
    TCL_SIZE_T             *shape;        /* Per-name arities, then row widths. */
    Tcl_Obj                *expandedSql, *expandedNames;
    Ns_DbBindParameterProc *marker;
    EncodedSql             *external, *expandedExternal;
    bool                    rewriteNames; /* Legacy $/# continuation names. */
} BindSql;

typedef enum {
    BindOptionNone,
    BindOptionVars,
    BindOptionSet,
    BindOptionDict,
    BindOptionTypes
} BindOptionType;

typedef struct BindOption {
    BindOptionType  source;
    Tcl_Obj        *value;
} BindOption;

/*
 * Function prototypes
 */

static void BindSpecsFree(BindSpec *specs, TCL_SIZE_T count);
static void EncodedSqlRelease(EncodedSql *encoded);
static void BindSqlRelease(BindSql *sql)
    NS_GNUC_NONNULL(1);
static bool BindNameIsStart(unsigned char c);
static bool BindNameIsChar(unsigned char c);
static int SqlObjCache(Tcl_Obj *obj, Tcl_Interp *interp)
    NS_GNUC_NONNULL(1,2);
static EncodedSql *BindSqlGetExternal(BindSql *sql, Tcl_Interp *interp, Tcl_Obj *statement, bool expanded)
    NS_GNUC_NONNULL(1,2,3);
static int BindSpecsParse(Tcl_Obj *types, Tcl_Interp *interp, TCL_SIZE_T count, Tcl_Obj *const *names, BindSpec **specsPtr, bool *hasExpansionPtr)
    NS_GNUC_NONNULL(1,2,5,6);
static int BindSqlExpand(BindSql *sql, Tcl_Interp *interp, Ns_DbHandle *handle,
                         Tcl_Obj *statement, TCL_SIZE_T count, const BindSpec *specs,
                         Tcl_Obj *const *values, bool emptyNull,
                         Tcl_Obj ***flatPtr, TCL_SIZE_T *flatCountPtr,
                         Ns_DbBindValueType **flatTypesPtr)
    NS_GNUC_NONNULL(1,2,3,4,6,7,9,10,11);
static int DbHandleReturnResult(Ns_DbHandle *handle, Tcl_Interp *interp, const char *operation, int status)
    NS_GNUC_NONNULL(1,2,3);

static Ns_ObjvProc BindOptionParse
    NS_GNUC_NONNULL(1,2,3,4);

static Tcl_FreeInternalRepProc SqlObjFree NS_GNUC_NONNULL(1);
static Tcl_DupInternalRepProc SqlObjDup NS_GNUC_NONNULL(1,2);

static CONST86 Tcl_ObjType sqlType = {
    "nsdb:bound-sql",
    SqlObjFree,
    SqlObjDup,
    NULL,
    NULL
#ifdef TCL_OBJTYPE_V0
   ,TCL_OBJTYPE_V0
#endif
};


/*
 *----------------------------------------------------------------------
 *
 * BindSpecsFree --
 *
 *      Release the interpretation metadata for count unique parameter names.
 *
 *      Each entry can own an optional row-column type array. A NULL specs
 *      pointer is accepted so the caller can use the same cleanup path
 *      before and after metadata validation.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees the column-type arrays and the specification array.
 *
 *----------------------------------------------------------------------
 */
static void
BindSpecsFree(BindSpec *specs, TCL_SIZE_T count)
{
    if (specs != NULL) {
        for (TCL_SIZE_T i = 0; i < count; i++) {
            ns_free(specs[i].columns);
        }
        ns_free(specs);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * EncodedSqlRelease --
 *
 *      Release one reference to an externally encoded SQL buffer.
 *
 *      Both a cache slot and an active driver invocation can own references.
 *      A NULL buffer is accepted for error cleanup; the final reference also
 *      releases the retained Tcl encoding handle.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Decrements the reference count and frees the buffer and encoding
 *      reference when no owners remain.
 *
 *----------------------------------------------------------------------
 */
static void
EncodedSqlRelease(EncodedSql *encoded)
{
    if (encoded != NULL && --encoded->refs == 0u) {
        Tcl_DStringFree(&encoded->bytes);
        Tcl_FreeEncoding(encoded->encoding);
        ns_free(encoded);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * BindSqlRelease --
 *
 *      Release one reference to a parsed SQL plan and its caches.
 *
 *      Tcl SQL objects and active bound commands retain the plan
 *      independently. Cached names, expansion shapes and external bytes
 *      remain available until the final plan reference is released.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      On the final reference, releases the retained Tcl objects,
 *      encoded-buffer references and allocated token/shape arrays.
 *
 *----------------------------------------------------------------------
 */
static void
BindSqlRelease(BindSql *sql)
{
    if (--sql->refs == 0u) {
        Tcl_DecrRefCount(sql->names);
        if (sql->expandedSql != NULL) {
            Tcl_DecrRefCount(sql->expandedSql);
        }
        if (sql->expandedNames != NULL) {
            Tcl_DecrRefCount(sql->expandedNames);
        }
        EncodedSqlRelease(sql->external);
        EncodedSqlRelease(sql->expandedExternal);
        ns_free(sql->occurrences);
        ns_free(sql->shape);
        ns_free(sql);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SqlObjFree --
 *
 *      Release the plan owned by a SQL Tcl_Obj internal representation.
 *
 *      This is the Tcl freeIntRepProc callback. Duplicated SQL objects and
 *      active bound commands can keep their own references to the same plan.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Releases the object's plan reference; the final owner frees the plan.
 *
 *----------------------------------------------------------------------
 */
static void
SqlObjFree(Tcl_Obj *obj)
{
    BindSqlRelease(obj->internalRep.twoPtrValue.ptr1);
}

/*
 *----------------------------------------------------------------------
 *
 * SqlObjDup --
 *
 *      Share the cached plan when Tcl duplicates a SQL object.
 *
 *      Copy only the internal representation from src to dst and retain the
 *      shared plan. Tcl manages the objects' string representations
 *      separately; token and external-buffer contents are not copied here.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments the plan reference count and initializes dst's internal
 *      representation and type.
 *
 *----------------------------------------------------------------------
 */
static void
SqlObjDup(Tcl_Obj *src, Tcl_Obj *dst)
{
    BindSql *sql = src->internalRep.twoPtrValue.ptr1;

    sql->refs++;
    dst->internalRep.twoPtrValue.ptr1 = sql;
    dst->internalRep.twoPtrValue.ptr2 = NULL;
    dst->typePtr = src->typePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * BindNameIsStart --
 *
 *      Test the first byte following a bind-parameter prefix.
 *
 *      Preserve ns_pg_bind's alphabetic-or-underscore rule. CHARTYPE applies
 *      C-locale byte classification, not Unicode character classification.
 *
 * Results:
 *      True for an alphabetic byte or underscore, otherwise false.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
BindNameIsStart(unsigned char c)
{
    return CHARTYPE(alpha, c) != 0 || c == '_';
}

/*
 *----------------------------------------------------------------------
 *
 * BindNameIsChar --
 *
 *      Test a subsequent byte of a bind-parameter name.
 *
 *      Preserve ns_pg_bind's alphanumeric, underscore, dollar-sign and
 *      number-sign continuation characters. A dollar sign within a name
 *      belongs to that name rather than starting a second parameter.
 *
 * Results:
 *      True for an accepted continuation byte, otherwise false.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
BindNameIsChar(unsigned char c)
{
    return CHARTYPE(alnum, c) != 0 || c == '_' || c == '$' || c == '#';
}

/*
 *----------------------------------------------------------------------
 *
 * SqlObjCache --
 *
 *      Parse and attach a binding plan on the first bound use of a SQL
 *      object.
 *
 *      Preserve obj's original SQL string. Record unique prefixed names and
 *      the offset of every occurrence, excluding supported quotes and
 *      comments. Legacy names requiring portable driver markers are flagged
 *      for cached rewriting. Statement boundaries, including triggers,
 *      remain the driver's responsibility.
 *
 *      This is a binding lexer rather than a complete SQL parser. PostgreSQL
 *      escape strings, dollar-quoted bodies and nested block comments are
 *      not implemented.
 *
 * Results:
 *      TCL_OK with the plan attached, or TCL_ERROR with an interpreter error
 *      result for rejected parameter syntax or unfinished quoting.
 *
 * Side effects:
 *      Replaces obj's internal representation and allocates a
 *      reference-counted token plan while retaining its original string
 *      bytes.
 *
 *----------------------------------------------------------------------
 */
static int
SqlObjCache(Tcl_Obj *obj, Tcl_Interp *interp)
{
    TCL_SIZE_T  length;
    const char *text = Tcl_GetStringFromObj(obj, &length);
    Tcl_Obj    *names = Tcl_NewListObj(0, NULL);
    char        quote = '\0';
    int         comment = 0;    /* 0: none, 1: line, 2: block. */
    BindSql    *sql = ns_calloc(1u, sizeof(BindSql));

    sql->refs = 1u;
    sql->names = names;
    Tcl_IncrRefCount(names);
    for (TCL_SIZE_T i = 0; i < length; i++) {
        char c = text[i], next = i + 1 < length ? text[i + 1] : '\0';

        if (comment == 1) {
            if (c == '\n') {
                comment = 0;
            }
            continue;
        }
        if (comment == 2) {
            if (c == '*' && next == '/') {
                comment = 0;
                i++;
            }
            continue;
        }
        if (quote != '\0') {
            if (c == quote) {
                if (next == quote && quote != ']') {
                    i++;
                } else {
                    quote = '\0';
                }
            }
            continue;
        }
        if (c == '-' && next == '-') {
            comment = 1;
            i++;
            continue;
        }
        if (c == '/' && next == '*') {
            comment = 2;
            i++;
            continue;
        }

        if (c == '\'' || c == '"' || c == '`' || c == '[') {
            quote = c == '[' ? ']' : c;

        } else if (c == '?' || (c == '@' && BindNameIsStart((unsigned char)next))) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("unrecognized or positional SQL parameter", TCL_INDEX_NONE));
            BindSqlRelease(sql);
            return TCL_ERROR;

        } else if ((c == ':' || c == '$') && BindNameIsStart((unsigned char)next)
                   && !(c == ':' && i > 0 && text[i - 1] == ':')) {
            TCL_SIZE_T  start = i, count, size, parameter;
            Tcl_Obj   **items;
            bool        found = NS_FALSE;

            while (i + 1 < length && BindNameIsChar((unsigned char)text[i + 1])) {
                i++;
                if (text[i] == '$' || text[i] == '#') {
                    sql->rewriteNames = NS_TRUE;
                }
            }
            size = i - start + 1;
            Tcl_ListObjGetElements(interp, names, &count, &items);
            parameter = count;
            for (TCL_SIZE_T j = 0; j < count; j++) {
                TCL_SIZE_T  n;
                const char *name = Tcl_GetStringFromObj(items[j], &n);

                if (n == size && memcmp(name, text + start, (size_t)n) == 0) {
                    found = NS_TRUE;
                    parameter = j;
                    break;
                }
            }
            if (!found) {
                Tcl_ListObjAppendElement(interp, names,
                                         Tcl_NewStringObj(text + start, size));
            }
            if ((size_t)sql->count == sql->capacity) {
                sql->capacity = sql->capacity == 0u ? 8u : sql->capacity * 2u;
                sql->occurrences = ns_realloc(sql->occurrences,
                                             sql->capacity * sizeof(BindOccurrence));
            }
            sql->occurrences[sql->count++] = (BindOccurrence){start, size, parameter};
        }
    }
    if (quote != '\0' || comment == 2) {
        BindSqlRelease(sql);
        Tcl_SetObjResult(interp,
                         Tcl_NewStringObj("unterminated SQL literal or comment", TCL_INDEX_NONE));
        return TCL_ERROR;
    }
    if (obj->typePtr != NULL && obj->typePtr->freeIntRepProc != NULL) {
        obj->typePtr->freeIntRepProc(obj);
    }
    obj->internalRep.twoPtrValue.ptr1 = sql;
    obj->internalRep.twoPtrValue.ptr2 = NULL;
    obj->typePtr = &sqlType;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * BindSqlGetExternal --
 *
 *      Obtain externally encoded bytes for the base or expanded SQL text.
 *
 *      Select the cache slot using expanded and encode statement only when
 *      that slot is absent or Tcl's system encoding has changed. Reject
 *      embedded NUL bytes before publishing a replacement; a failed
 *      conversion leaves the previous slot intact.
 *
 *      The returned buffer has a reference owned by the caller, which must
 *      call EncodedSqlRelease after use. This pins its bytes even if a
 *      nested driver invocation replaces the shared cache slot.
 *
 * Results:
 *      An owned EncodedSql reference, or NULL with an interpreter error
 *      result when encoding lookup or NUL validation fails.
 *
 * Side effects:
 *      May allocate and replace one cache entry. Retains an encoding handle
 *      and the caller's buffer reference.
 *
 *----------------------------------------------------------------------
 */
static EncodedSql *
BindSqlGetExternal(BindSql *sql, Tcl_Interp *interp, Tcl_Obj *statement,
                   bool expanded)
{
    EncodedSql  **slot = expanded ? &sql->expandedExternal : &sql->external;
    Tcl_Encoding  encoding = Tcl_GetEncoding(interp, NULL);
    EncodedSql   *encoded;
    TCL_SIZE_T    length;
    const char   *text;

    if (encoding == NULL) {
        return NULL;
    }
    if (*slot != NULL && (*slot)->encoding == encoding) {
        Tcl_FreeEncoding(encoding);
        (*slot)->refs++;
        return *slot;
    }
    encoded = ns_calloc(1u, sizeof(EncodedSql));
    encoded->refs = 1u;
    encoded->encoding = encoding;
    text = Tcl_GetStringFromObj(statement, &length);
    Tcl_UtfToExternalDString(encoding, text, length, &encoded->bytes);
    if (strlen(encoded->bytes.string) != (size_t)encoded->bytes.length) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("SQL contains NUL character", TCL_INDEX_NONE));
        EncodedSqlRelease(encoded);
        return NULL;
    }
    EncodedSqlRelease(*slot);
    *slot = encoded;
    encoded->refs++;
    return encoded;
}

/*
 *----------------------------------------------------------------------
 *
 * BindSpecsParse --
 *
 *      Validate explicit -bindtypes metadata for count parameter names.
 *
 *      Match dictionary keys against names without their bind prefixes and
 *      validate scalar, list, rows and binary interpretations. Row metadata
 *      can supply one interpretation per column. Unspecified parameters
 *      retain the zero-initialized scalar interpretation.
 *
 *      On success, transfer the allocated specification array through
 *      specsPtr and mark hasExpansionPtr when a non-scalar interpretation
 *      requires expansion. Free partially constructed metadata on failure.
 *
 * Results:
 *      TCL_OK on success, or TCL_ERROR with the invalid type, unused name or
 *      malformed row metadata reported in the interpreter.
 *
 * Side effects:
 *      Allocates specifications and optional column-type arrays. Transfers
 *      specsPtr on success; the expansion flag may be set during validation.
 *      Frees partial allocations on failure.
 *
 *----------------------------------------------------------------------
 */
static int
BindSpecsParse(Tcl_Obj *types, Tcl_Interp *interp, TCL_SIZE_T count,
               Tcl_Obj *const *names, BindSpec **specsPtr, bool *hasExpansionPtr)
{
    Tcl_DictSearch  search;
    Tcl_Obj       *key, *value;
    int            done;
    BindSpec      *specs = ns_calloc((size_t)count + 1u, sizeof(BindSpec));

    if (Tcl_DictObjFirst(interp, types, &search, &key, &value, &done) != TCL_OK) {
        BindSpecsFree(specs, count);
        return TCL_ERROR;
    }
    for (; !done; Tcl_DictObjNext(&search, &key, &value, &done)) {
        Tcl_Obj     **parts;
        TCL_SIZE_T    n, index = count;
        BindValueType kind;
        const char   *type;

        if (Tcl_ListObjGetElements(interp, value, &n, &parts) != TCL_OK) {
            goto error;
        }
        type = n > 0 ? Tcl_GetString(parts[0]) : "";
        if (strcmp(type, "scalar") == 0) {
            kind = BindScalar;
        } else if (strcmp(type, "list") == 0) {
            kind = BindList;
        } else if (strcmp(type, "rows") == 0) {
            kind = BindRows;
        } else if (strcmp(type, "binary") == 0) {
            kind = BindBinary;
        } else {
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("unsupported bind type '%s': expected scalar, list, rows or binary", type));
            goto error;
        }

        if (n != 1 && !(kind == BindRows && n == 2)) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("expected a bind type or {rows {column-types}}", TCL_INDEX_NONE));
            goto error;
        }
        for (TCL_SIZE_T i = 0; i < count; i++) {
            if (strcmp(Tcl_GetString(key), Tcl_GetString(names[i]) + 1) == 0) {
                index = i;
                break;
            }
        }
        if (index == count) {
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("bind type names unused parameter '%s'", Tcl_GetString(key)));
            goto error;
        }
        specs[index].kind = kind;
        if (n == 2) {
            Tcl_Obj   **columns;
            TCL_SIZE_T  columnsCount;

            if (Tcl_ListObjGetElements(interp, parts[1], &columnsCount, &columns) != TCL_OK) {
                goto error;
            }
            if (columnsCount == 0) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("row column types must not be empty", TCL_INDEX_NONE));
                goto error;
            }
            specs[index].columns = ns_calloc((size_t)columnsCount, sizeof(Ns_DbBindValueType));
            specs[index].columnCount = columnsCount;
            for (TCL_SIZE_T i = 0; i < columnsCount; i++) {
                const char *column = Tcl_GetString(columns[i]);

                if (strcmp(column, "binary") == 0) {
                    specs[index].columns[i] = NS_DB_BIND_BINARY;
                }
                else if (strcmp(column, "scalar") != 0) {
                    Tcl_SetObjResult(interp,
                                     Tcl_NewStringObj("row column types must be scalar or binary", TCL_INDEX_NONE));
                    goto error;
                }
            }
        }
        if (kind != BindScalar) {
            *hasExpansionPtr = NS_TRUE;
        }
    }
    Tcl_DictObjDone(&search);
    *specsPtr = specs;
    return TCL_OK;

 error:
    Tcl_DictObjDone(&search);
    BindSpecsFree(specs, count);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * BindSqlExpand --
 *
 *      Expand current parameter values into driver-compatible binding slots.
 *
 *      Validate list arities and row widths, then cache one SQL expansion
 *      per shape and driver marker factory. Every original parameter
 *      receives private slot names, including scalars in rewritten
 *      statements, so source names cannot collide with generated names.
 *      Repeated occurrences reuse slots without reparsing SQL.
 *
 *      Read values, list elements and row cells anew for every execution.
 *      Return allocated flat-value and flat-type arrays through the output
 *      pointers; Tcl_Obj pointers in the value array are borrowed from
 *      values and its retained collections, not independently referenced
 *      here.
 *
 * Results:
 *      TCL_OK with caller-owned flat arrays and count, or TCL_ERROR with a
 *      validation/marker error in the interpreter.
 *
 * Side effects:
 *      May replace the plan's expanded SQL, names, shape and encoded cache.
 *      Allocates the flat arrays; the caller must retain source values while
 *      using them and free the arrays afterwards.
 *
 *----------------------------------------------------------------------
 */
static int
BindSqlExpand(BindSql *sql, Tcl_Interp *interp, Ns_DbHandle *handle,
              Tcl_Obj *statement, TCL_SIZE_T count, const BindSpec *specs,
              Tcl_Obj *const *values, bool emptyNull,
              Tcl_Obj ***flatPtr, TCL_SIZE_T *flatCountPtr,
              Ns_DbBindValueType **flatTypesPtr)
{
    TCL_SIZE_T             *arity = ns_malloc(((size_t)count + 1u) * sizeof(TCL_SIZE_T));
    TCL_SIZE_T             *width = ns_calloc((size_t)count + 1u, sizeof(TCL_SIZE_T));
    TCL_SIZE_T             *offset = ns_malloc(((size_t)count + 1u) * sizeof(TCL_SIZE_T));
    TCL_SIZE_T              total = 0;
    Tcl_Obj               **flat = NULL;
    Ns_DbBindValueType     *flatTypes;
    Ns_DbBindParameterProc *marker = NsDbGetBindParameterProc(handle);

    /*
     * Validate all collection shapes before changing cached SQL or returning
     * flat values. Each name has one starting offset in the resulting array.
     */
    for (TCL_SIZE_T i = 0; i < count; i++) {
        arity[i] = 1;
        offset[i] = total;
        if (specs[i].kind == BindList || specs[i].kind == BindRows) {
            Tcl_Obj **items;

            if (values[i] == NULL) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("NULL is not a list binding", TCL_INDEX_NONE));
                goto error;
            }
            if (Tcl_ListObjGetElements(interp, values[i], &arity[i], &items) != TCL_OK) {
                goto error;
            }
            if (arity[i] == 0) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("empty list binding is not supported", TCL_INDEX_NONE));
                goto error;
            }
        }
        if (specs[i].kind == BindRows) {
            Tcl_Obj  **rows;
            TCL_SIZE_T n;

            Tcl_ListObjGetElements(interp, values[i], &n, &rows);
            for (TCL_SIZE_T j = 0; j < n; j++) {
                TCL_SIZE_T columns;

                if (Tcl_ListObjLength(interp, rows[j], &columns) != TCL_OK) {
                    goto error;
                }
                if (columns == 0 || (j > 0 && columns != width[i])) {
                    Tcl_SetObjResult(interp,
                                     Tcl_NewStringObj("rows binding requires nonempty rows of equal width", TCL_INDEX_NONE));
                    goto error;
                }
                width[i] = columns;
            }
            if (specs[i].columnCount > 0 && specs[i].columnCount != width[i]) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("row width does not match declared column types", TCL_INDEX_NONE));
                goto error;
            }
            if (width[i] > INT_MAX / arity[i]) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("too many expanded bind values", TCL_INDEX_NONE));
                goto error;
            }
            arity[i] *= width[i];
        }
        if (arity[i] > INT_MAX - total) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("too many expanded bind values", TCL_INDEX_NONE));
            goto error;
        }
        total += arity[i];
    }
    if (sql->shape == NULL || sql->marker != marker
        || memcmp(sql->shape, arity, (size_t)count * sizeof(TCL_SIZE_T)) != 0
        || memcmp(sql->shape + count, width, (size_t)count * sizeof(TCL_SIZE_T)) != 0) {
        Tcl_Obj     *expandedNames = Tcl_NewListObj(0, NULL), *expandedSql;
        TCL_SIZE_T   length, previous = 0;
        const char  *text = Tcl_GetStringFromObj(statement, &length);
        Tcl_DString  buffer;

        Tcl_IncrRefCount(expandedNames);
        for (TCL_SIZE_T i = 0; i < total; i++) {
            Tcl_Obj *name = marker == NULL
                ? Tcl_ObjPrintf(":b%" TCL_LL_MODIFIER "d", (Tcl_WideInt)i)
                : marker(i);

            if (name == NULL) {
                Tcl_DecrRefCount(expandedNames);
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("driver could not create a bind marker", TCL_INDEX_NONE));
                goto error;
            }
            Tcl_ListObjAppendElement(interp, expandedNames, name);
        }
        Tcl_DStringInit(&buffer);
        for (TCL_SIZE_T i = 0; i < sql->count; i++) {
            const BindOccurrence *token = &sql->occurrences[i];

            Tcl_DStringAppend(&buffer, text + previous, token->offset - previous);
            for (TCL_SIZE_T j = 0; j < arity[token->parameter]; j++) {
                Tcl_Obj   *name;
                TCL_SIZE_T columns = width[token->parameter];

                if (j > 0) {
                    Tcl_DStringAppend(&buffer, ",", 1);
                }
                if (columns > 0 && j % columns == 0) {
                    Tcl_DStringAppend(&buffer, "(", 1);
                }
                Tcl_ListObjIndex(interp, expandedNames,
                                 offset[token->parameter] + j, &name);
                Tcl_DStringAppend(&buffer, Tcl_GetString(name), TCL_INDEX_NONE);
                if (columns > 0 && j % columns == columns - 1) {
                    Tcl_DStringAppend(&buffer, ")", 1);
                }
            }
            previous = token->offset + token->length;
        }
        Tcl_DStringAppend(&buffer, text + previous, length - previous);
        expandedSql = Tcl_NewStringObj(buffer.string, buffer.length);
        Tcl_IncrRefCount(expandedSql);
        Tcl_DStringFree(&buffer);
        if (sql->expandedSql != NULL) {
            Tcl_DecrRefCount(sql->expandedSql);
        }
        if (sql->expandedNames != NULL) {
            Tcl_DecrRefCount(sql->expandedNames);
        }
        EncodedSqlRelease(sql->expandedExternal);
        sql->expandedExternal = NULL;
        sql->marker = marker;
        sql->expandedSql = expandedSql;
        sql->expandedNames = expandedNames;
        if (sql->shape == NULL) {
            sql->shape = ns_malloc(2u * (size_t)count * sizeof(TCL_SIZE_T));
        }
        memcpy(sql->shape, arity, (size_t)count * sizeof(TCL_SIZE_T));
        memcpy(sql->shape + count, width, (size_t)count * sizeof(TCL_SIZE_T));
    }
    /*
     * Materialize only the pointer/type arrays. Elements remain borrowed from
     * the caller's retained values, and no value is stored in the SQL cache.
     */
    flat = ns_calloc((size_t)total + 1u, sizeof(Tcl_Obj *));
    flatTypes = ns_calloc((size_t)total + 1u, sizeof(Ns_DbBindValueType));
    for (TCL_SIZE_T i = 0; i < count; i++) {
        if (specs[i].kind == BindList || specs[i].kind == BindRows) {
            Tcl_Obj  **items;
            TCL_SIZE_T n;

            Tcl_ListObjGetElements(interp, values[i], &n, &items);
            for (TCL_SIZE_T j = 0; j < n; j++) {
                if (width[i] > 0) {
                    Tcl_Obj  **cells;
                    TCL_SIZE_T columns;

                    Tcl_ListObjGetElements(interp, items[j], &columns, &cells);
                    for (TCL_SIZE_T k = 0; k < columns; k++) {
                        TCL_SIZE_T         slot = offset[i] + j * columns + k;
                        Ns_DbBindValueType type = specs[i].columns == NULL
                            ? NS_DB_BIND_TEXT : specs[i].columns[k];

                        flatTypes[slot] = type;
                        flat[slot] = cells[k];
                        if (emptyNull) {
                            TCL_SIZE_T length;

                            if (type == NS_DB_BIND_BINARY) {
                                Tcl_GetByteArrayFromObj(cells[k], &length);
                            } else {
                                Tcl_GetStringFromObj(cells[k], &length);
                            }
                            if (length == 0) {
                                flat[slot] = NULL;
                            }
                        }
                    }
                } else {
                    TCL_SIZE_T length = 1;

                    if (emptyNull) {
                        Tcl_GetStringFromObj(items[j], &length);
                    }
                    flat[offset[i] + j] = length == 0 ? NULL : items[j];
                }
            }
        } else {
            flat[offset[i]] = values[i];
            if (specs[i].kind == BindBinary) {
                flatTypes[offset[i]] = NS_DB_BIND_BINARY;
            }
        }
    }
    ns_free(width);
    ns_free(arity);
    ns_free(offset);
    *flatPtr = flat;
    *flatCountPtr = total;
    *flatTypesPtr = flatTypes;
    return TCL_OK;

 error:
    ns_free(width);
    ns_free(arity);
    ns_free(offset);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * DbHandleReturnResult --
 *
 *      Translate a driver status to the requested ns_db command result.
 *
 *      Keep exec, dml, select, 1row and 0or1row result shapes and
 *      cardinality checks consistent with their ordinary interfaces. select
 *      returns a handle-owned set; one-row commands return an owned copy so
 *      their result survives flushing the driver's row state.
 *
 * Results:
 *      TCL_OK after setting the appropriate interpreter result, or TCL_ERROR
 *      for driver failure, an incompatible result shape or a row-count
 *      error.
 *
 * Side effects:
 *      May fetch and flush rows, update active-result state, register a Tcl
 *      set handle and report a database exception.
 *
 *----------------------------------------------------------------------
 */
static int
DbHandleReturnResult(Ns_DbHandle *handle, Tcl_Interp *interp,
                     const char *operation, int status)
{
    Ns_Set *row = NULL, *copy = NULL;
    int     first, second;

    if (status != NS_DML && status != NS_ROWS) {
        goto error;
    }
    if (strcmp(operation, "exec") == 0) {
        Tcl_SetObjResult(interp,
                         Tcl_NewStringObj(status == NS_DML ? "NS_DML" : "NS_ROWS", TCL_INDEX_NONE));
        return TCL_OK;
    }
    if (strcmp(operation, "dml") == 0) {
        if (status == NS_DML) {
            return TCL_OK;
        }
        Ns_DbSetException(handle, "NSDB", "Query was not a DML or DDL command.");
        (void)Ns_DbFlush(handle);
        goto error;
    }
    if (status != NS_ROWS) {
        Ns_DbSetException(handle, "NSDB", "Query was not a SELECT command.");
        goto error;
    }
    row = Ns_DbBindRow(handle);
    if (row == NULL) {
        goto error;
    }
    NsDbSetActive("bound select", handle, NS_TRUE);
    if (strcmp(operation, "select") == 0) {
        return Ns_TclEnterSet(interp, row, NS_TCL_SET_STATIC);
    }
    first = Ns_DbGetRow(handle, row);
    if (first == NS_ERROR) {
        (void)Ns_DbFlush(handle);
        goto error;
    }
    if (first == NS_END_DATA) {
        (void)Ns_DbFlush(handle);
        if (strcmp(operation, "0or1row") == 0) {
            return TCL_OK;
        }
        Ns_DbSetException(handle, "NSDB", "Query did not return a row.");
        goto error;
    }
    copy = Ns_SetCopy(row);
    second = Ns_DbGetRow(handle, row);
    (void)Ns_DbFlush(handle);
    if (second != NS_END_DATA) {
        Ns_SetFree(copy);
        if (second == NS_OK) {
            Ns_DbSetException(handle, "NSDB", "Query returned more than one row.");
        }
        goto error;
    }
    if (Ns_TclEnterSet(interp, copy, NS_TCL_SET_DYNAMIC) != TCL_OK) {
        Ns_SetFree(copy);
        return TCL_ERROR;
    }
    return TCL_OK;

 error:
    NsDbSetActive("SQL bind vars error", handle, NS_FALSE);
    if (Tcl_GetStringResult(interp)[0] == '\0') {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Database operation \"%s\" failed: %s",
                                               operation, handle->dsExceptionMsg.string));
    }
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * BindOptionParse --
 *
 *      Parse a binding source or type dictionary with Ns_ParseObjv.
 *
 *      Binding sources share a destination, so repeated sources are also
 *      mutually exclusive. A separate destination rejects repeated type
 *      dictionaries. The option type identifies a flag (caller variables)
 *      or a value-bearing option (set, dictionary or type metadata).
 *
 * Results:
 *      TCL_OK, or TCL_ERROR with an option validation error.
 *
 * Side effects:
 *      Records the source and borrows the option value from objv.
 *
 *----------------------------------------------------------------------
 */
static int
BindOptionParse(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
                Tcl_Obj *const *objv)
{
    BindOption     *option = spec->dest;
    BindOptionType  source = (BindOptionType)PTR2INT(spec->arg);

    if (option->source != BindOptionNone) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
                             source == BindOptionTypes
                             ? "-bindtypes may only be supplied once"
                             : "binding sources are mutually exclusive",
                             TCL_INDEX_NONE));
        return TCL_ERROR;
    }
    if (source != BindOptionVars) {
        Ns_ObjvSpec valueSpec = {spec->key, Ns_ObjvObj, &option->value, NULL};

        if (Ns_ObjvObj(&valueSpec, interp, objcPtr, objv) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    option->source = source;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsDbBindObjCmd --
 *
 *      Implement opt-in binding and capability queries on existing ns_db
 *      commands.
 *
 *      Interpret objc/objv using operation to select bindinfo, exec, dml,
 *      select, 1row or 0or1row behavior. Binding accepts exactly one source:
 *      caller variables, an ns_set or a dictionary. Resolve current values
 *      on every invocation; missing names always fail, and -emptynull maps
 *      only explicitly supplied empty values.
 *
 *      Retain the SQL plan across Tcl read traces and representation
 *      changes. Apply typed collection/binary metadata, reuse cached driver
 *      markers when required and pin encoded SQL until the driver call
 *      completes. Values are passed separately from SQL to the ordinary
 *      driver timing and result-handling path.
 *
 * Results:
 *      TCL_OK with capability information or the normal command result,
 *      otherwise TCL_ERROR with an interpreter error result.
 *
 * Side effects:
 *      May invoke variable read traces, update SQL caches, execute SQL,
 *      fetch/flush rows and change database state. Releases temporary
 *      arrays, value references and plan/buffer references before returning.
 *
 *----------------------------------------------------------------------
 */
int
NsDbBindObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const objv[],
               const char *operation)
{
    Ns_DbHandle        *handle;
    Ns_Set             *set = NULL;
    Tcl_Obj            *dict = NULL, *types = NULL, *statement, *nameList, **names, **values;
    Tcl_Obj            *executeSql, *executeNames, **executeNameItems, **executeValues;
    TCL_SIZE_T          executeCount, i, count;
    BindSpec           *specs = NULL;
    Ns_DbBindValueType *executeTypes = NULL;
    bool                hasCollections = NS_FALSE, expand;
    BindOptionType      source = BindOptionNone;
    int                 result = TCL_ERROR, status;
    int                 emptyNull = NS_FALSE;
    BindSql            *sql;
    EncodedSql         *encoded;

    const char         *handleName;
    BindOption          binding = {BindOptionNone, NULL}, typeOption = {BindOptionNone, NULL};
    Ns_ObjvSpec         handleArgs[] = {
        {"handle", Ns_ObjvString, &handleName, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec         opts[] = {
        {"-bindvars",  BindOptionParse, &binding,    INT2PTR(BindOptionVars)},
        {"-bindset",   BindOptionParse, &binding,    INT2PTR(BindOptionSet)},
        {"-binddict",  BindOptionParse, &binding,    INT2PTR(BindOptionDict)},
        {"-bindtypes", BindOptionParse, &typeOption, INT2PTR(BindOptionTypes)},
        {"-emptynull", Ns_ObjvBool,   &emptyNull,  INT2PTR(NS_TRUE)},
        {"--",        Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec         args[] = {
        {"sql", Ns_ObjvObj, &statement, NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
     * The established command syntax places binding options after handle.
     * Parse that positional prefix before the operation-specific options.
     */
    if (Ns_ParseObjv(NULL, handleArgs, interp, 2, MIN(objc, 3), objv) != NS_OK
        || Ns_TclDbGetHandle(interp, handleName, &handle) != TCL_OK) {
        return TCL_ERROR;
    }
    if (strcmp(operation, "bindinfo") == 0) {
        const char *version = NULL;
        bool        supported = Ns_DbBindSupported(handle);
        Ns_ObjvSpec infoArgs[] = {
            {"?-version", Ns_ObjvString, &version, NULL},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(NULL, infoArgs, interp, 3, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }
        if (version != NULL && strcmp(version, "-version") != 0) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle ?-version?");
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, version != NULL
                         ? Tcl_NewIntObj(supported ? 5 : 0)
                         : Tcl_NewBooleanObj(supported));
        return TCL_OK;
    }
    if (Ns_ParseObjv(opts, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    source = binding.source;
    types = typeOption.value;
    if (source == BindOptionNone) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
                             "one binding source is required: -bindvars, -bindset or -binddict",
                             TCL_INDEX_NONE));
        return TCL_ERROR;
    }
    if (source == BindOptionSet
        && Ns_TclGetSet2(interp, Tcl_GetString(binding.value), &set) != TCL_OK) {
        return TCL_ERROR;
    }
    if (source == BindOptionDict) {
        dict = binding.value;
    }
    if (dict != NULL) {
        TCL_SIZE_T size;

        if (Tcl_DictObjSize(interp, dict, &size) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    if (statement->typePtr != &sqlType && SqlObjCache(statement, interp) != TCL_OK) {
        return TCL_ERROR;
    }
    sql = statement->internalRep.twoPtrValue.ptr1;
    /*
     * Read traces and list conversion can shimmer the SQL object. Retain its
     * complete token plan independently of the object's internal rep.
     */
    sql->refs++;
    nameList = sql->names;
    Tcl_IncrRefCount(nameList);
    Tcl_ListObjGetElements(interp, nameList, &count, &names);
    values = ns_calloc((size_t)count + 1u, sizeof(Tcl_Obj *));
    if (types != NULL
        && BindSpecsParse(types, interp, count, names, &specs, &hasCollections) != TCL_OK) {
        goto cleanup;
    }

    for (i = 0; i < count; i++) {
        const char *name = Tcl_GetString(names[i]) + 1;
        Tcl_Obj    *valueObj = NULL;

        if (source == BindOptionVars) {
            valueObj = Tcl_GetVar2Ex(interp, name, NULL, TCL_LEAVE_ERR_MSG);
            if (valueObj == NULL) {
                goto cleanup;
            }
        } else if (source == BindOptionSet) {
            ssize_t     index = Ns_SetFind(set, name);
            const char *value;

            if (index < 0) {
                goto missing;
            }
            value = Ns_SetValue(set, (size_t)index);
            if (value == NULL) {
                continue;
            }
            valueObj = Tcl_NewStringObj(value, TCL_INDEX_NONE);
        } else {
            Tcl_Obj *key = Tcl_NewStringObj(name, TCL_INDEX_NONE);

            Tcl_IncrRefCount(key);
            status = Tcl_DictObjGet(interp, dict, key, &valueObj);
            Tcl_DecrRefCount(key);
            if (status != TCL_OK) {
                goto cleanup;
            }
            if (valueObj == NULL) {
                goto missing;
            }
        }
        Tcl_IncrRefCount(valueObj);
        values[i] = valueObj;
        if (emptyNull
            && (specs == NULL || specs[i].kind == BindScalar || specs[i].kind == BindBinary)) {
            TCL_SIZE_T length;

            if (specs != NULL && specs[i].kind == BindBinary) {
                Tcl_GetByteArrayFromObj(values[i], &length);
            }
            else {
                Tcl_GetStringFromObj(values[i], &length);
            }
            if (length == 0) {
                Tcl_DecrRefCount(values[i]);
                values[i] = NULL;
            }
        }
        continue;
    missing:
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("undefined bind variable '%s'", name));
        goto cleanup;
    }
    executeSql = statement;
    executeNames = nameList;
    executeValues = values;
    executeCount = count;
    /*
     * Drivers need not accept every legacy bind-name character. Reuse cached
     * slot rewriting for these names as well as for collection bindings.
     */
    expand = hasCollections || sql->rewriteNames;
    if (expand && specs == NULL) {
        specs = ns_calloc((size_t)count + 1u, sizeof(BindSpec));
    }
    if (expand && BindSqlExpand(sql, interp, handle, statement, count, specs, values,
                                emptyNull, &executeValues, &executeCount, &executeTypes) != TCL_OK) {
        goto cleanup;
    }
    if (expand) {
        executeSql = sql->expandedSql;
        executeNames = sql->expandedNames;
    }
    Tcl_IncrRefCount(executeSql);
    Tcl_IncrRefCount(executeNames);
    Tcl_ListObjGetElements(interp, executeNames, &executeCount, &executeNameItems);
    encoded = BindSqlGetExternal(sql, interp, executeSql, expand);
    if (encoded != NULL) {
        bool binary = NS_FALSE;

        Tcl_DStringFree(&handle->dsExceptionMsg);
        handle->cExceptionCode[0] = '\0';
        Tcl_ResetResult(interp);

        for (TCL_SIZE_T j = 0; executeTypes != NULL && j < executeCount; j++) {
            if (executeTypes[j] == NS_DB_BIND_BINARY) {
                binary = NS_TRUE;
                break;
            }
        }
        status = binary
            ? Ns_DbBindTypedExec(interp, handle, encoded->bytes.string, executeCount,
                                 executeNameItems, executeValues, executeTypes)
            : Ns_DbBindExec(interp, handle, encoded->bytes.string, executeCount,
                            executeNameItems, executeValues);
        result = DbHandleReturnResult(handle, interp, operation, status);
    }
    EncodedSqlRelease(encoded);
    Tcl_DecrRefCount(executeSql);
    Tcl_DecrRefCount(executeNames);
    if (executeValues != values) {
        ns_free(executeValues);
    }

 cleanup:
    for (TCL_SIZE_T j = 0; j < count; j++) {
        if (values[j] != NULL) {
            Tcl_DecrRefCount(values[j]);
        }
    }
    ns_free(values);
    BindSpecsFree(specs, count);
    ns_free(executeTypes);
    Tcl_DecrRefCount(nameList);
    BindSqlRelease(sql);
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
