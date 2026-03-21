/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

/*
 * tcljson.c --
 *
 *       This file implements NaviServer's native JSON support, providing
 *       strict RFC 8259 parsing, lossless round-trips, explicit typing, and
 *       efficient integration with Tcl data structures.
 *
 *       The implementation emphasizes predictable behavior, precise error
 *       reporting, and performance optimizations such as shared JSON object
 *       keys to reduce memory footprint and allocation overhead.
 */

#include "nsd.h"

#ifndef NS_JSON_KEY_SHARING
# define NS_JSON_KEY_SHARING 1
#endif

/*
 * math.h is needed for JsonParseNumber and the macros NS_ISFINITE, NS_ISINF,
 * and NS_ISNAN
 */
#include <math.h>

#if defined(_MSC_VER)
# define NS_ISFINITE(x) _finite(x)
# define NS_ISINF(x)    (!_finite(x) && !_isnan(x))
# define NS_ISNAN(x)    _isnan(x)
#else
# define NS_ISFINITE(x) isfinite(x)
# define NS_ISINF(x)    isinf(x)
# define NS_ISNAN(x)    isnan(x)
#endif

#ifndef TCL_HASH_TYPE
#if TCL_MAJOR_VERSION > 8
#  define TCL_HASH_TYPE size_t
#else
#  define TCL_HASH_TYPE unsigned
#endif
#endif

#define NS_JSON_NULL_STRING ""
#define NS_JSON_NULL_STRING_LEN (sizeof(NS_JSON_NULL_STRING) - 1)

#define NS_JSON_TYPE_SUFFIX ".type"
#define NS_JSON_TYPE_SUFFIX_LEN (sizeof(NS_JSON_TYPE_SUFFIX) - 1)

typedef enum {
    JSON_VT_AUTO = 0u, /* internal/unset; not a real JSON type */
    JSON_VT_STRING,
    JSON_VT_NUMBER,   /* numeric lexeme, as-is */
    JSON_VT_BOOL,
    JSON_VT_NULL,
    JSON_VT_OBJECT,   /* triples */
    JSON_VT_ARRAY     /* triples */
} JsonValueType;

typedef enum {
    JSON_ATOM_UNUSED = 0,          /* matches JSON_VT_AUTO */

    JSON_ATOM_T_STRING,
    JSON_ATOM_T_NUMBER,
    JSON_ATOM_T_BOOLEAN,
    JSON_ATOM_T_NULL,
    JSON_ATOM_T_OBJECT,
    JSON_ATOM_T_ARRAY,

    /* non-type atoms after that */
    JSON_ATOM_TRUE,
    JSON_ATOM_FALSE,
    JSON_ATOM_EMPTY,
    JSON_ATOM_VALUE_NULL_STRING,

    JSON_ATOM_KEY,
    JSON_ATOM_FIELD,

    /* JSON Schema helper atoms */

    JSON_ATOM_TYPE,        /* "type" */
    JSON_ATOM_PROPERTIES,  /* "properties" */
    JSON_ATOM_REQUIRED,    /* "required" */
    JSON_ATOM_ITEMS,       /* "items" */
    JSON_ATOM_ANYOF,       /* "anyOf" */
    JSON_ATOM_SCHEMA,      /* "$schema" */

    JSON_ATOM_TITLE,       /* "title */
    JSON_ATOM_DESCRIPTION, /* "description */
    JSON_ATOM_DEFAULT,     /* "default */
    JSON_ATOM_EXAMPLES,    /* "examples */
    JSON_ATOM_BOOL,        /* "bool */

    JSON_ATOM_MAX
} JsonAtom;

static const NsAtomSpec jsonAtomSpecs[JSON_ATOM_MAX] = {
    /* Keep indices aligned with JsonAtom enum order */

    /* JSON_ATOM_UNUSED (matches JSON_VT_AUTO) */
    {-1, "auto", 4},

    /* type atoms (MUST align with JsonValueType) */
    {-1, "string",  6},  /* JSON_ATOM_T_STRING */
    {-1, "number",  6},  /* JSON_ATOM_T_NUMBER */
    {-1, "boolean", 7},  /* JSON_ATOM_T_BOOLEAN */
    {-1, "null",    4},  /* JSON_ATOM_T_NULL */
    {-1, "object",  6},  /* JSON_ATOM_T_OBJECT */
    {-1, "array",   5},  /* JSON_ATOM_T_ARRAY */

    /* non-type atoms */
    {NS_ATOM_TRUE,    NULL, 0},  /* JSON_ATOM_TRUE */
    {NS_ATOM_FALSE,   NULL, 0},  /* JSON_ATOM_FALSE */
    {NS_ATOM_EMPTY,   NULL, 0},  /* JSON_ATOM_EMPTY */
    {-1, NS_JSON_NULL_STRING, NS_JSON_NULL_STRING_LEN}, /* JSON_ATOM_VALUE_NULL_STRING */

    /* misc */
    {-1, "key",   3},           /* JSON_ATOM_KEY */
    {-1, "field", 5},           /* JSON_ATOM_FIELD */

    {NS_ATOM_TYPE,  NULL, 0},   /* JSON_ATOM_TYPE */
    {-1, "properties",   10},   /* JSON_ATOM_PROPERTIES */
    {-1, "required",      8},   /* JSON_ATOM_REQUIRED */
    {-1, "items",         5},   /* JSON_ATOM_ITEMS */
    {-1, "anyOf",         5},   /* JSON_ATOM_ANYOF */
    {-1, "$schema",       7},   /* JSON_ATOM_SCHEMA */

    {-1, "title",         5},   /* JSON_ATOM_TITLE */
    {-1, "description",  11},   /* JSON_ATOM_DESCRIPTION */
    {-1, "default",       7},   /* JSON_ATOM_TITLE */
    {-1, "examples",      8},   /* JSON_ATOM_EXAMPLES */
    {-1, "bool",          4}    /* JSON_ATOM_BOOL */

};
static Tcl_Obj *JsonAtomObjs[JSON_ATOM_MAX];

/*
 * Parser state
 */
typedef struct JsonParser {
    Tcl_Interp            *interp;

    const unsigned char   *start;
    const unsigned char   *p;
    const unsigned char   *end;

    const Ns_JsonOptions *opt;
    long                  depth;
    //size_t              nKeyAlloc;
    //size_t              nKeyFree;
    size_t                nKeyReuse;
    size_t                nKeyObjIncr;
    //size_t              nKeyObjDecr;
    //size_t              nKeyAllocDropped;

    Tcl_HashTable         keyTable;
    Tcl_DString          *errDsPtr;
    /*
     * Scratch for string construction; avoids lots of small allocations.
     * (Optional; can also build via Tcl_AppendToObj.)
     */
    Tcl_DString           tmpDs;
} JsonParser;

/*
 * JsonKey --
 *
 *      Key object used both for lookups (stack-allocated) and as the stored
 *      hash table key (heap-allocated). The bytes pointer may point either
 *      into the JSON buffer (lookup) or into the allocated tail storage
 *      (stored key).
 */
typedef struct JsonKey {
    TCL_SIZE_T  len;
    const char *bytes;
} JsonKey;

/*
 * Ranges for numeric options.
 */
static Ns_ObjvValueRange posIntRange1 = {1, INT_MAX};   /* >= 1 */
static Ns_ObjvValueRange posIntRange0 = {0, INT_MAX};   /* >= 0 */

/*
 * Enumeration tables for command options
 */
static Ns_ObjvTable outputFormats[] = {
    {"tclvalue", NS_JSON_OUTPUT_TCL_VALUE},
    {"dict",     NS_JSON_OUTPUT_TCL_VALUE}, /* legacy alias kept for prerelease compatibility */
    {"triples",  NS_JSON_OUTPUT_TRIPLES},
    {"set",      NS_JSON_OUTPUT_NS_SET},
    {NULL,       0u}
};
static Ns_ObjvTable topModes[] = {
    {"any",       NS_JSON_TOP_ANY},
    {"container", NS_JSON_TOP_CONTAINER},
    {NULL,        0u}
};

static Ns_ObjvTable jsonValueTypes[] = {
    {"auto",     JSON_VT_AUTO},
    {"string",   JSON_VT_STRING},
    {"number",   JSON_VT_NUMBER},
    {"boolean",  JSON_VT_BOOL},
    {"bool",     JSON_VT_BOOL},
    {"null",     JSON_VT_NULL},
    {"object",   JSON_VT_OBJECT},
    {"array",    JSON_VT_ARRAY},
    {NULL,       0u}
};
typedef enum {
    JSON_OUT_JSON = 0,
    JSON_OUT_TRIPLES
} JsonOutputMode;

static Ns_ObjvTable outputModeTable[] = {
    {"json",    JSON_OUT_JSON},
    {"triples", JSON_OUT_TRIPLES},
    {NULL, 0}
};

/*
 * Structs for sorting
 */
typedef struct {
    Tcl_Obj *nameObj;
    Tcl_Obj *typeObj;
    Tcl_Obj *valueObj;
} JsonTripleEntry;

typedef struct {
    Tcl_Obj *valueObj;
} JsonStringEntry;

/*
 * Local functions defined in this file
 */
static TCL_OBJCMDPROC_T JsonIsNullObjCmd;
static TCL_OBJCMDPROC_T JsonKeyDecodeObjCmd;
static TCL_OBJCMDPROC_T JsonKeyEncodeObjCmd;
static TCL_OBJCMDPROC_T JsonKeyInfoObjCmd;
static TCL_OBJCMDPROC_T JsonNullObjCmd;
static TCL_OBJCMDPROC_T JsonParseObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesGettypeObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesGetvalueObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesMatchObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesSchemaObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesSetvalueObjCmd;
static TCL_OBJCMDPROC_T JsonValueObjCmd;

static Tcl_HashKeyProc JsonHashKeyProc;
static Tcl_CompareHashKeysProc JsonCompareKeysProc;

static Tcl_DupInternalRepProc JsonNullDupIntRep;
static Tcl_UpdateStringProc JsonNullUpdateString;
static Tcl_FreeInternalRepProc JsonNullFreeIntRep;

static Tcl_AllocHashEntryProc JsonAllocEntryProc;
static Tcl_FreeHashEntryProc JsonFreeEntryProc;

static const Tcl_ObjType JsonNullObjType = {
    "jsonNull",
    JsonNullFreeIntRep,   /* freeIntRepProc */
    JsonNullDupIntRep,    /* dupIntRepProc */
    JsonNullUpdateString, /* updateStringProc */
    NULL                  /* setFromAnyProc (explicit construction only) */
#ifdef TCL_OBJTYPE_V0
   ,TCL_OBJTYPE_V0
#endif
};

/*
 * Descriptor for custom hash table
 */
static const Tcl_HashKeyType JsonKeyType = {
    TCL_HASH_KEY_TYPE_VERSION,
    0,               /* flags */
    JsonHashKeyProc,
    JsonCompareKeysProc,
    JsonAllocEntryProc,
    JsonFreeEntryProc
};

/*
 * Static functions defined in this file.
 */

/*
 * JSON atom helpers.
 */
void                 NsAtomJsonInit(void);
static bool          JsonObjIsAtom(Tcl_Obj *obj, JsonAtom atom)  NS_GNUC_NONNULL(1) NS_GNUC_PURE;
/*
 * Null value helpers.
 */
static Tcl_Obj      *JsonNewNullObj(Tcl_Obj *labelObj) NS_GNUC_NONNULL(1);
static bool          JsonIsNullObj(Tcl_Obj *valueObj) NS_GNUC_NONNULL(1);

/*
 * Generic object helper and string encoding helpers.
 */
static Tcl_Obj      *DStringToObj(Tcl_DString *dsPtr)  NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonAppendDecoded(JsonParser *jp, const unsigned char *at, const char *bytes, size_t len) NS_GNUC_NONNULL(1,2,3);
static void          JsonAppendQuotedString(Tcl_DString *dsPtr, const char *s, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);

/*
 * Low-level byte/scan helpers.
 */
static uint64_t      JsonEqByteMask(uint64_t x, uint64_t y) NS_GNUC_CONST;
static int           JsonWordAllWs(uint64_t w) NS_GNUC_CONST;
static uint64_t      JsonHasByteLt0x20(uint64_t x) NS_GNUC_CONST;
static const unsigned char *JsonFindCtlLt0x20(const unsigned char *p, const unsigned char *end) NS_GNUC_NONNULL(1,2) NS_GNUC_PURE;
static const unsigned char *JsonSkipWsPtr(const unsigned char *p, const unsigned char *end) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonCheckNoCtlInString(JsonParser *jp, const unsigned char *p, const unsigned char *end) NS_GNUC_NONNULL(1,2,3);

/*
 * Parser cursor helpers.
 */
static void          JsonSkipWs(JsonParser *jp) NS_GNUC_NONNULL(1);
static int           JsonPeek(JsonParser *jp) NS_GNUC_NONNULL(1);
static int           JsonGet(JsonParser *jp) NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonExpect(JsonParser *jp, int ch, const char *what) NS_GNUC_NONNULL(1,3);

/*
 * Number and escape decoding helpers.
 */
static Ns_ReturnCode JsonDecodeHex4(JsonParser *jp, uint16_t *u16Ptr) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonDecodeUnicodeEscape(JsonParser *jp, uint32_t *cpPtr) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonScanNumber(JsonParser *jp, const unsigned char **startPtr,  const unsigned char **endPtr, bool *sawFracOrExpPtr)
    NS_GNUC_NONNULL(1,2,3,4);
static bool          JsonNumberLexemeIsValid(const unsigned char *s, size_t len) NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonValidateNumberString(const unsigned char *s, size_t len, Ns_DString *errDsPtr) NS_GNUC_NONNULL(1,3);
static Ns_ReturnCode JsonNumberObjToLexeme(Tcl_Interp *interp, Tcl_Obj *valueObj, Tcl_Obj **outObjPtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonCheckStringSpan(JsonParser *jp, const unsigned char *p, const unsigned char *q, size_t outLen)
    NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonValidateValue(Tcl_Interp *interp, JsonValueType vt, Tcl_Obj *inObj, Tcl_Obj **outObjPtr, const char *what)
    NS_GNUC_NONNULL(1,3,4,5);

/*
 * Key sharing/interning helpers.
 */
static JsonKey      *JsonKeyAlloc(JsonParser *jp, const char *bytes, TCL_SIZE_T len) NS_GNUC_NONNULL(2);
static Tcl_Obj      *JsonInternKeyObj(JsonParser *jp, const char *bytes, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);

/*
 * Type detection and triples/container validation.
 */
static const char *  JsonTypeString(JsonValueType vt);
static JsonValueType JsonTypeObjToVt(Tcl_Obj *typeObj) NS_GNUC_NONNULL(1);
static JsonValueType JsonValueTypeDetect(Tcl_Interp *interp, Tcl_Obj *valueObj) NS_GNUC_NONNULL(1,2);
static JsonValueType TriplesDetectRootWrapper(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj **rootTypeObjPtr, Tcl_Obj **rootValuePtr)  NS_GNUC_NONNULL(1,2,4);
static Ns_ReturnCode JsonRequireValidNumberObj(Tcl_Interp *interp, Tcl_Obj *valueObj) NS_GNUC_NONNULL(2);
static Ns_ReturnCode JsonTriplesRequireValidContainerObj(Tcl_Interp *interp, Tcl_Obj *containerObj,
                                                         bool allowEmpty, bool validateNumbers,
                                                         const char *what) NS_GNUC_NONNULL(1,2,5);

/*
 * Triple appending.
 */
static inline void   JsonTriplesAppend(Tcl_Obj *triplesObj, Tcl_Obj *nameObj, Tcl_Obj *typeObj, Tcl_Obj *valueObj) NS_GNUC_NONNULL(1,2,3,4);
static inline void   JsonTriplesAppendVt(Tcl_Obj *triplesObj, Tcl_Obj *nameObj, JsonValueType vt, Tcl_Obj *valueObj) NS_GNUC_NONNULL(1,2,4);

/*
 * Triples/path lookup and update helpers.
 */
static Ns_ReturnCode TriplesLookupPath(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *triplesObj,
                                       Tcl_Obj **valuePtr, Tcl_Obj **typePtr, JsonValueType *vtPtr,
                                       Tcl_Obj **valueIndexPathPtr, Tcl_Obj **typeIndexPathPtr) NS_GNUC_NONNULL(1,2,3);
static bool          TripleKeyMatches(Tcl_Obj *keyObj, Tcl_Obj *segObj)  NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode TriplesFind(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *segObj, TCL_SIZE_T *tripleBaseIdxPtr) NS_GNUC_NONNULL(1,2,3,4);
static Tcl_Obj      *JsonPointerToPathObj(Tcl_Interp *interp, const char *p, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonTriplesGetPath(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *pointerObj, Tcl_Obj **outPathObj) NS_GNUC_NONNULL(1,4);
static Ns_ReturnCode TriplesSetValue(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *triplesObj, Tcl_Obj *newValueObj,
                                     JsonValueType vt, Tcl_Obj **resultTriplesPtr) NS_GNUC_NONNULL(1,2,3,4,6);

/*
 * Core parse helpers.
 */
static Ns_ReturnCode JsonParseLiteral(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseNumber(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseStringToString(JsonParser *jp, const char **outPtr, TCL_SIZE_T *outLenPtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseString(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseKeyString(JsonParser *jp, Tcl_Obj **keyObjPtr) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonParseValue(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseObject(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)  NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseArray(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)  NS_GNUC_NONNULL(1,2,3);

/*
 * Set-output parse helpers.
 */
static Ns_ReturnCode JsonParseValueSet(JsonParser *jp, Ns_Set *set, Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                       JsonValueType *valueTypePtr, bool *emptyPtr) NS_GNUC_NONNULL(1,2,3,4,5,6);
static Ns_ReturnCode JsonParseObjectSet(JsonParser *jp, Ns_Set *set, Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                        bool *emptyPtr) NS_GNUC_NONNULL(1,2,3,4,5);
static Ns_ReturnCode JsonParseArraySet(JsonParser *jp, Ns_Set *set,
                                       Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                       bool *emptyPtr) NS_GNUC_NONNULL(1,2,3,4,5);
/*
 * Set population helpers.
 */
static void          JsonSetPutValue(Ns_Set *set, const char *key, TCL_SIZE_T keyLen, const char *val, TCL_SIZE_T valLen) NS_GNUC_NONNULL(1,2,4);
static void          JsonSetPutType(Ns_Set *set, Tcl_DString *typeKeyDsPtr, const char *key, TCL_SIZE_T keyLen, JsonValueType vt) NS_GNUC_NONNULL(1,2,3);
static void          JsonFlattenToSet(Ns_Set *set, const Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                      Tcl_Obj *valueObj, const char *valStr, TCL_SIZE_T valLen, JsonValueType vt) NS_GNUC_NONNULL(1,2,3);
/*
 * Key/path helpers for set and pointer handling.
 */
static TCL_SIZE_T    JsonKeySplitSidecarField(const char *key, TCL_SIZE_T keyLen,
                                           const char **fieldPtr, TCL_SIZE_T *fieldLenPtr) NS_GNUC_NONNULL(1,3,4);
static void          JsonKeyPathEscapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen, bool rfc6901) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonKeyPathUnescapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen, bool rfc6901) NS_GNUC_NONNULL(1,2);
static void          JsonKeyPathAppendSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen) NS_GNUC_NONNULL(1,2);
static void          JsonKeyPathAppendIndex(Tcl_DString *dsPtr, size_t idx) NS_GNUC_NONNULL(1);
static void          JsonKeyPathMakeTypeKey(Tcl_DString *typeKeyDsPtr, const char *key, TCL_SIZE_T keyLen) NS_GNUC_NONNULL(1,2);

/*
 * JSON emission helpers.
 */
static int           JsonTriplesValueToJson(Tcl_Interp *interp, JsonValueType vt, Tcl_Obj *valueObj,
                                            bool pretty, bool validateNumbers, Tcl_Obj **outObjPtr) NS_GNUC_NONNULL(1,3,6);
static int           JsonEmitValueFromTriple(Tcl_Interp *interp, Tcl_Obj *typeObj, Tcl_Obj *valObj,
                                             bool validateNumbers, int depth, bool pretty,
                                             Tcl_DString *dsPtr) NS_GNUC_NONNULL(1,2,3,7);
static int           JsonEmitContainerFromTriples(Tcl_Interp *interp, Tcl_Obj *triplesObj, bool isObject,
                                                  bool validateNumbers, int depth, bool pretty,
                                                  Tcl_DString *dsPtr) NS_GNUC_NONNULL(1,2,7);
/*
 * Pretty-print helpers.
 */
static void          JsonPrettyIndent(Tcl_DString *dsPtr, int depth) NS_GNUC_NONNULL(1);
static bool          JsonObjectPrettySingleLine(Tcl_Interp *interp, TCL_SIZE_T oc, Tcl_Obj **ov) NS_GNUC_NONNULL(1,3);
static bool          JsonArrayPrettySingleLine(TCL_SIZE_T oc, Tcl_Obj **ov) NS_GNUC_NONNULL(2);

/*
 * Sorting helpers.
 */
static Ns_ReturnCode JsonTriplesSortObject(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj **sortedObjPtr) NS_GNUC_NONNULL(1,2,3);
static int           JsonTripleEntryNameCmp(const void *a, const void *b) NS_GNUC_NONNULL(1,2);
static int           JsonSchemaStringEntryCmp(const void *a, const void *b) NS_GNUC_NONNULL(1,2);

/*
 * Schema triples field access helpers.
 */
static bool          JsonSchemaFindField(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *nameObj, TCL_SIZE_T *baseIdxPtr) NS_GNUC_NONNULL(1,2,3,4);
static bool          JsonSchemaGetField(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *nameObj, Tcl_Obj **typeObjPtr, Tcl_Obj **valueObjPtr, TCL_SIZE_T *baseIdxPtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonSchemaSetField(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *nameObj, JsonValueType vt, Tcl_Obj *valueObj) NS_GNUC_NONNULL(1,2,3,5);

/*
 * Schema derivation helpers.
 */
static Tcl_Obj      *JsonSchemaNewTypeTriple(JsonAtom atom);
static Ns_ReturnCode JsonSchemaFromValue(Tcl_Interp *interp, JsonValueType vt, Tcl_Obj *valueObj, bool includeRequired, Tcl_Obj **schemaObjPtr) NS_GNUC_NONNULL(1,3,5);
static Ns_ReturnCode JsonSchemaFromObject(Tcl_Interp *interp, Tcl_Obj *triplesObj, bool includeRequired, Tcl_Obj **schemaObjPtr) NS_GNUC_NONNULL(1,2,4);
static Ns_ReturnCode JsonSchemaFromArray(Tcl_Interp *interp, Tcl_Obj *triplesObj, bool includeRequired, Tcl_Obj **schemaObjPtr) NS_GNUC_NONNULL(1,2,4);

/*
 * Schema merge helpers.
 */
static Ns_ReturnCode JsonSchemaMerge(Tcl_Interp *interp, Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj, Tcl_Obj **mergedObjPtr) NS_GNUC_NONNULL(1,2,3,4);
static Ns_ReturnCode JsonSchemaMergeObject(Tcl_Interp *interp, Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj, Tcl_Obj **mergedObjPtr) NS_GNUC_NONNULL(1,2,3,4);
static Ns_ReturnCode JsonSchemaMergeArray(Tcl_Interp *interp, Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj, Tcl_Obj **mergedObjPtr) NS_GNUC_NONNULL(1,2,3,4);

/*
 * Schema canonicalization helpers.
 */
static Ns_ReturnCode JsonSchemaCanonicalize(Tcl_Interp *interp, Tcl_Obj *schemaObj, Tcl_Obj **canonObjPtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonSchemaCanonicalizeTypeField(Tcl_Interp *interp, Tcl_Obj *typeTypeObj, Tcl_Obj *typeValueObj, Tcl_Obj **outTypeObjPtr) NS_GNUC_NONNULL(1,2,3,4);
static Ns_ReturnCode JsonSchemaCanonicalizeRequired(Tcl_Interp *interp, Tcl_Obj *requiredObj, Tcl_Obj **canonRequiredObjPtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonSchemaCanonicalizeProperties(Tcl_Interp *interp, Tcl_Obj *propertiesObj, Tcl_Obj **canonPropsObjPtr) NS_GNUC_NONNULL(1,2,3);

/*
 * Schema mismatch reporting helpers.
 */
static Ns_ReturnCode JsonSchemaMismatchType(Tcl_Interp *interp, Tcl_DString *pathDsPtr, const char *expected, const char *actual)  NS_GNUC_NONNULL(1,2,3,4);
static Ns_ReturnCode JsonSchemaMismatchTypeUnion(Tcl_Interp *interp, Tcl_DString *pathDsPtr, Tcl_Obj *typeObj, const char *actual) NS_GNUC_NONNULL(1,2,3,4);
static Ns_ReturnCode JsonSchemaMismatchMissingRequired(Tcl_Interp *interp, Tcl_DString *pathDsPtr, Tcl_Obj *nameObj) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonSchemaMismatchUnexpectedProperty(Tcl_Interp *interp, Tcl_DString *pathDsPtr)  NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonSchemaMismatchAnyOf(Tcl_Interp *interp, Tcl_DString *pathDsPtr, JsonValueType actualVt) NS_GNUC_NONNULL(1,2);

/*
 * JSON Pointer path construction helpers.
 */
static void          JsonPointerPathPushKey(Tcl_DString *dsPtr, Tcl_Obj *keyObj) NS_GNUC_NONNULL(1,2);
static void          JsonPointerPathPushIndex(Tcl_DString *dsPtr, TCL_SIZE_T idx)  NS_GNUC_NONNULL(1);
static void          JsonPointerPathPop(Tcl_DString *dsPtr, TCL_SIZE_T oldLen) NS_GNUC_NONNULL(1);

/*
 * Schema validation/matching helpers.
 */
static const char *  JsonSchemaMisMatchGetPath(Tcl_DString *pathDsPtr) NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonSchemaRequireSupportedSubset(Tcl_Interp *interp, Tcl_Obj *schemaObj, bool ignoreUnsupported) NS_GNUC_NONNULL(1,2);
static Tcl_Obj      *JsonSchemaDictGet(Tcl_Obj *schemaObj, JsonAtom atom) NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonSchemaMatchValue(Tcl_Interp *interp, Tcl_Obj *schemaObj, JsonValueType actualVt, Tcl_Obj *actualObj, Tcl_DString *pathDsPtr) NS_GNUC_NONNULL(1,2,4,5);
static Ns_ReturnCode JsonSchemaMatchType(Tcl_Interp *interp, Tcl_Obj *typeObj, JsonValueType actualVt, Tcl_DString *pathDsPtr);
static Ns_ReturnCode JsonSchemaMatchAnyOf(Tcl_Interp *interp, Tcl_Obj *anyOfObj, JsonValueType actualVt, Tcl_Obj *actualObj, Tcl_DString *pathDsPtr) NS_GNUC_NONNULL(1,2,4,5);
static Ns_ReturnCode JsonSchemaMatchObject(Tcl_Interp *interp, Tcl_Obj *schemaObj, Tcl_Obj *triplesObj, Tcl_DString *pathDsPtr) NS_GNUC_NONNULL(1,2,3,4);
static Ns_ReturnCode JsonSchemaMatchArray(Tcl_Interp *interp, Tcl_Obj *schemaObj, Tcl_Obj *triplesObj, Tcl_DString *pathDsPtr) NS_GNUC_NONNULL(1,2,3,4);

/*
 * Schema type helpers.
 */
static bool          JsonSchemaIsTypeOnlySchema(Tcl_Interp *interp, Tcl_Obj *schemaObj, Tcl_Obj **typeTypeObjPtr, Tcl_Obj **typeValueObjPtr) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonSchemaExtractTypeUnion(Tcl_Interp *interp, Tcl_Obj *schemaObj, Tcl_Obj **typesObjPtr) NS_GNUC_NONNULL(1,2,3);
static bool          JsonSchemaTypeUnionContains(Tcl_Interp *interp, Tcl_Obj *typesObj, Tcl_Obj *typeNameObj) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonSchemaMergeTypeUnion(Tcl_Interp *interp, Tcl_Obj *types1Obj, Tcl_Obj *types2Obj, Tcl_Obj **mergedTypesObjPtr) NS_GNUC_NONNULL(1,2,3,4);
static Ns_ReturnCode JsonSchemaBuildTypeUnion(Tcl_Interp *interp, Tcl_Obj *typesObj, Tcl_Obj **schemaObjPtr) NS_GNUC_NONNULL(1,2,3);
static Tcl_Obj      *JsonSchemaBuildAnyOf2(Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj) NS_GNUC_NONNULL(1,2);

/*
 * Schema required-field helpers.
 */
static Ns_ReturnCode JsonSchemaRequiredIntersection(Tcl_Interp *interp, Tcl_Obj *required1Obj, Tcl_Obj *required2Obj, Tcl_Obj **requiredOutObjPtr) NS_GNUC_NONNULL(1,4);
static bool          JsonSchemaRequiredContains(Tcl_Interp *interp, Tcl_Obj *requiredObj, Tcl_Obj *nameObj) NS_GNUC_NONNULL(1,2,3);

/*
 * Result post-processing helpers.
 */
static int           JsonMaybeWrapScanResult(Tcl_Interp *interp, bool isScan, size_t consumed) NS_GNUC_NONNULL(1);
static int           JsonCheckTrailingDecode(Tcl_Interp *interp, const unsigned char *buf, size_t len, size_t consumed) NS_GNUC_NONNULL(1,2);


/*======================================================================
 * Function Implementations: JSON atom initialization.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * NsJsonInitAtoms --
 *
 *      Initialize the process-wide atom objects used by the JSON parser and
 *      generator. The atom table provides canonical Tcl_Obj pointers for JSON
 *      value types and frequently used literals (e.g.,
 *      true/false/null/empty), avoiding repeated allocations and string
 *      conversions in hot paths.
 *
 *      This function is expected to be called once during module/library
 *      initialization before any JSON parsing or emission uses the atom
 *      pointers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Creates Tcl objects for the atom table entries and increments their
 *      reference counts so they remain valid for the lifetime of the
 *      process/interpreter as intended by the implementation.
 *
 *----------------------------------------------------------------------
 */
void
NsAtomJsonInit(void)
{
    (void) NsAtomsInit(jsonAtomSpecs, JSON_ATOM_MAX, JsonAtomObjs);
    /*
     * Replace the default NULL atom with a dedicated JsonNullObjType object.
     * The singleton is stored in the atom table, so callers can safely use
     * JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING] for typed null without allocating
     * fresh objects.
     */
    Tcl_DecrRefCount(JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING]);
    JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING] =
        JsonNewNullObj(Tcl_NewStringObj(NS_JSON_NULL_STRING, NS_JSON_NULL_STRING_LEN));
    Tcl_IncrRefCount(JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING]);

}


/*======================================================================
 * Function Implementations: NullObjType
 *======================================================================
 */

/*
 * ----------------------------------------------------------------------
 *
 * JsonNullObjType --
 *
 *      Tcl object type representing JSON null.
 *
 *      The internal representation stores a label Tcl_Obj* (typically a
 *      string) which defines the string representation of this null object.
 *      This allows "ns_json parse -nullvalue <value>" to materialize JSON
 *      null as a typed Tcl object while still using a caller-chosen string
 *      representation.
 *
 *      Important:
 *        - Null detection must be based on ObjType, not string matching.
 *        - setFromAnyProc is NULL: the string "null" is not implicitly
 *          converted to JsonNullObjType.
 *
 *      internalRep.twoPtrValue.ptr1 : Tcl_Obj* label (IncrRefCount'd)
 *      internalRep.twoPtrValue.ptr2 : unused (NULL)
 *
 *----------------------------------------------------------------------
 */
static void
JsonNullFreeIntRep(Tcl_Obj *objPtr)
{
    Tcl_Obj *labelObj = (Tcl_Obj *)objPtr->internalRep.twoPtrValue.ptr1;

    if (labelObj != NULL) {
        Tcl_DecrRefCount(labelObj);
        objPtr->internalRep.twoPtrValue.ptr1 = NULL;
    }
    objPtr->internalRep.twoPtrValue.ptr2 = NULL;
}

static void
JsonNullDupIntRep(Tcl_Obj *srcPtr, Tcl_Obj *dupPtr)
{
    Tcl_Obj *labelObj = (Tcl_Obj *)srcPtr->internalRep.twoPtrValue.ptr1;

    dupPtr->internalRep.twoPtrValue.ptr1 = (void *)labelObj;
    dupPtr->internalRep.twoPtrValue.ptr2 = NULL;

    if (labelObj != NULL) {
        Tcl_IncrRefCount(labelObj);
    }

    dupPtr->typePtr = &JsonNullObjType;
}

static void
JsonNullUpdateString(Tcl_Obj *objPtr)
{
    Tcl_Obj   *labelObj = (Tcl_Obj *)objPtr->internalRep.twoPtrValue.ptr1;
    TCL_SIZE_T len = 0;
    const char *s;

    if (labelObj == NULL) {
        s = "null";
        len = 4;
    } else {
        s = Tcl_GetStringFromObj(labelObj, &len);
    }

    objPtr->bytes = (char *)ckalloc((unsigned)len + 1u);
    memcpy(objPtr->bytes, s, (size_t)len);
    objPtr->bytes[len] = '\0';
    objPtr->length = (int)len;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonNewNullObj --
 *
 *      Create a new Tcl object representing JSON null.
 *
 *      The returned object uses the dedicated JSON-null Tcl object type
 *      (JsonNullObjType).  The provided labelObj defines the string
 *      representation of the null object.  This allows JSON null values
 *      parsed with the "-nullvalue" option to retain a caller-defined
 *      textual representation while still being identifiable via the
 *      JSON-null object type.
 *
 *      The label object is retained in the internal representation of the
 *      returned object and its reference count is incremented accordingly.
 *
 * Results:
 *      A newly allocated Tcl_Obj of type JsonNullObjType.
 *
 * Side effects:
 *      Increments the reference count of labelObj.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
JsonNewNullObj(Tcl_Obj *labelObj)
{
    Tcl_Obj *objPtr = Tcl_NewObj();

    Tcl_InvalidateStringRep(objPtr);

    objPtr->internalRep.twoPtrValue.ptr1 = NULL;
    objPtr->internalRep.twoPtrValue.ptr2 = NULL;

    Tcl_IncrRefCount(labelObj);
    objPtr->internalRep.twoPtrValue.ptr1 = (void *)labelObj;

    objPtr->typePtr = &JsonNullObjType;

    return objPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonIsNullObj --
 *
 *      Determine whether the provided Tcl object represents a JSON null
 *      value as used internally by ns_json.
 *
 *      JSON null values are represented by a dedicated Tcl object type
 *      (JsonNullObjType).  This function checks the object's type pointer
 *      and therefore distinguishes JSON null values from ordinary Tcl
 *      strings such as "null", the empty string, or other values that may
 *      appear when parsing JSON with a custom "-nullvalue" option.
 *
 * Results:
 *      true  when the object has type JsonNullObjType,
 *      false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonIsNullObj(Tcl_Obj *valueObj)
{
    return (valueObj->typePtr == &JsonNullObjType);
}



/*======================================================================
 * Function Implementation: Generic object helper and string encoding helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * DStringToObj --
 *
 *      This function moves a dynamic string's contents to a new Tcl_Obj. Be
 *      aware that this function does *not* check that the encoding of the
 *      contents of the dynamic string is correct; this is the caller's
 *      responsibility to enforce.
 *
 *      The function is essentially a copy of the internal function
 *      TclDStringToOb and is a counter part to Tcl_DStringResult.
 *
 * Results:
 *      The newly-allocated untyped (i.e., typePtr==NULL) Tcl_Obj with a
 *      reference count of zero.
 *
 * Side effects:
 *      The string is "moved" to the object. dsPtr is reinitialized to an
 *      empty string; it does not need to be Tcl_DStringFree'd after this if
 *      not used further.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
DStringToObj(Tcl_DString *dsPtr)
{
    Tcl_Obj *result;

    if (dsPtr->string == dsPtr->staticSpace) {
        if (dsPtr->length == 0) {
            result = Tcl_NewObj();
        } else {
            /*
             * Static buffer, so must copy.
             */
            result = Tcl_NewStringObj(dsPtr->string, dsPtr->length);
        }
    } else {
        /*
         * Dynamic buffer, so transfer ownership and reset.
         */
        result = Tcl_NewObj();
        result->bytes = dsPtr->string;
        result->length = dsPtr->length;
    }

    /*
     * Re-establish the DString as empty with no buffer allocated.
     */
    dsPtr->string = dsPtr->staticSpace;
    dsPtr->spaceAvl = TCL_DSTRING_STATIC_SIZE;
    dsPtr->length = 0;
    dsPtr->staticSpace[0] = '\0';

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonAppendDecoded --
 *
 *      Append decoded bytes to the temporary buffer used while unescaping
 *      JSON string values and enforce the configured maximum decoded string
 *      length.
 *
 *      The parameter 'at' identifies a position in the input stream and is
 *      used solely for reporting accurate byte offsets in error messages.
 *
 * Results:
 *      NS_OK on success.
 *      NS_ERROR if appending would exceed jp->opt->maxString.
 *
 * Side effects:
 *      On success, extends jp->tmpDs.
 *      On failure, appends an explanatory error message to jp->errDsPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonAppendDecoded(JsonParser *jp, const unsigned char *at, const char *bytes, size_t len)
{
    if (jp->opt->maxString > 0u
        && (size_t)jp->tmpDs.length + len > (size_t)jp->opt->maxString) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: string too long (max %lu)",
                         (unsigned long)(at - jp->start),
                         (unsigned long)jp->opt->maxString);
        return NS_ERROR;
    }
    Tcl_DStringAppend(&jp->tmpDs, bytes, (TCL_SIZE_T)len);
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonAppendQuotedString --
 *
 *      Append a JSON string literal for the provided byte sequence to the
 *      destination DString.  The result is surrounded by double quotes and
 *      contains the required JSON escape sequences for quotes, backslashes,
 *      and control characters.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Appends bytes to dsPtr.
 *
 *----------------------------------------------------------------------
 */
static void
JsonAppendQuotedString(Tcl_DString *dsPtr, const char *s, TCL_SIZE_T len)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + len;

    Tcl_DStringAppend(dsPtr, "\"", 1);

    while (p < end) {
        unsigned char c = *p++;
        switch (c) {
        case '\"': Tcl_DStringAppend(dsPtr, "\\\"", 2); break;
        case '\\': Tcl_DStringAppend(dsPtr, "\\\\", 2); break;
        case '\b': Tcl_DStringAppend(dsPtr, "\\b", 2); break;
        case '\f': Tcl_DStringAppend(dsPtr, "\\f", 2); break;
        case '\n': Tcl_DStringAppend(dsPtr, "\\n", 2); break;
        case '\r': Tcl_DStringAppend(dsPtr, "\\r", 2); break;
        case '\t': Tcl_DStringAppend(dsPtr, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char buf[7];
                (void)snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                Tcl_DStringAppend(dsPtr, buf, 6);
            } else {
                Tcl_DStringAppend(dsPtr, (const char *)&c, 1);
            }
            break;
        }
    }

    Tcl_DStringAppend(dsPtr, "\"", 1);
}


/*======================================================================
 * Function Implementation: Low-level byte/scan helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonEqByteMask --
 *
 *      Build a byte-wise equality mask for the given machine word.
 *
 *      For each byte in `word`, the function compares it to `byteValue`
 *      and returns a word-sized mask where bytes that are equal are set
 *      to 0xFF and bytes that are not equal are set to 0x00. This is a
 *      low-level helper used for word-at-a-time scans (e.g., whitespace
 *      skipping) without branching per character.
 *
 * Results:
 *      A word-sized mask with 0xFF in each byte position where the input
 *      word byte equals `byteValue`, and 0x00 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline uint64_t
JsonEqByteMask(uint64_t x, uint64_t y)
{
    uint64_t v = x ^ y;
    return (~v & (v - 0x0101010101010101ULL) & 0x8080808080808080ULL);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonWordAllWs --
 *
 *      Determine whether all bytes in the provided machine word are JSON
 *      whitespace characters.
 *
 *      The JSON whitespace set is: space (' '), tab ('\t'), carriage return
 *      ('\r'), and newline ('\n'). This predicate is intended for fast
 *      word-at-a-time scanning in JsonSkipWsPtr() and related routines.
 *
 * Results:
 *      NS_TRUE if every byte in `word` is JSON whitespace, NS_FALSE
 *      otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline int
JsonWordAllWs(uint64_t w)
{
    uint64_t m;

    m  = JsonEqByteMask(w, 0x2020202020202020ULL); /* ' ' */
    m |= JsonEqByteMask(w, 0x0909090909090909ULL); /* '\t' */
    m |= JsonEqByteMask(w, 0x0A0A0A0A0A0A0A0AULL); /* '\n' */
    m |= JsonEqByteMask(w, 0x0D0D0D0D0D0D0D0DULL); /* '\r' */

    return (m == 0x8080808080808080ULL);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonHasByteLt0x20 --
 *
 *      Fast predicate to determine whether a byte sequence contains any
 *      ASCII control character below 0x20 (space). This is used primarily
 *      to validate JSON strings, where unescaped control characters are
 *      not permitted.
 *
 * Results:
 *      NS_TRUE if the buffer contains at least one byte < 0x20,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline uint64_t
JsonHasByteLt0x20(uint64_t x)
{
    const uint64_t n = 0x2020202020202020ULL;
    return ((x - n) & ~x & 0x8080808080808080ULL);
}
/*
 *----------------------------------------------------------------------
 *
 * JsonFindCtlLt0x20 --
 *
 *      Scan a byte sequence and return a pointer to the first occurrence
 *      of an ASCII control character below 0x20 (space).
 *
 *      This helper is used for error reporting (pinpointing the offending
 *      byte) and for validating that JSON strings do not contain unescaped
 *      control characters.
 *
 * Results:
 *      Pointer to the first byte < 0x20 within the buffer, or NULL if no
 *      such byte exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline const unsigned char *
JsonFindCtlLt0x20(const unsigned char *p, const unsigned char *end)
{
    const unsigned char *cur = p;

    /*
     * Fast scan: 32-byte chunks.
     * This returns only "maybe"; we still pinpoint below.
     */
    for (; cur + 32 <= end; cur += 32) {
        uint64_t w[4];
        uint64_t hit;

        memcpy(w, cur, sizeof(w));
        hit  = JsonHasByteLt0x20(w[0]);
        hit |= JsonHasByteLt0x20(w[1]);
        hit |= JsonHasByteLt0x20(w[2]);
        hit |= JsonHasByteLt0x20(w[3]);

        if (unlikely(hit != 0u)) {
            goto pinpoint;
        }
    }

    for (; cur + 8 <= end; cur += 8) {
        uint64_t w;
        memcpy(&w, cur, sizeof(w));
        if (unlikely(JsonHasByteLt0x20(w) != 0u)) {
            goto pinpoint;
        }
    }

    for (; cur < end; cur++) {
        if (unlikely(*cur < 0x20u)) {
            return cur;
        }
    }
    return NULL;

pinpoint:
    /*
     * Pinpoint first offending byte.
     * We intentionally restart at p for a correct earliest position.
     * (This is very rare in practice.)
     */
    for (cur = p; cur < end; cur++) {
        if (*cur < 0x20u) {
            return cur;
        }
    }
    return NULL; /* should not happen */
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSkipWsPtr --
 *
 *      Skip over JSON whitespace starting at the byte pointer `p` and stop
 *      at the first non-whitespace byte or at `end`.
 *
 *      Whitespace recognized is the JSON set: space, tab, carriage return,
 *      and newline.
 *
 * Results:
 *      Pointer to the first non-whitespace byte in [p..end), or `end` when
 *      only whitespace (or nothing) remains.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline const unsigned char *
JsonSkipWsPtr(const unsigned char *p, const unsigned char *end)
{
    /* Fast 32-byte blocks */
    while (p + 32 <= end) {
        uint64_t w0, w1, w2, w3;

        memcpy(&w0, p +  0, sizeof(w0));
        memcpy(&w1, p +  8, sizeof(w1));
        memcpy(&w2, p + 16, sizeof(w2));
        memcpy(&w3, p + 24, sizeof(w3));

        if (unlikely(!JsonWordAllWs(w0))) break;
        if (unlikely(!JsonWordAllWs(w1))) { p +=  8; break; }
        if (unlikely(!JsonWordAllWs(w2))) { p += 16; break; }
        if (unlikely(!JsonWordAllWs(w3))) { p += 24; break; }

        p += 32;
    }

    /* 8-byte blocks */
    while (p + 8 <= end) {
        uint64_t w;
        memcpy(&w, p, sizeof(w));
        if (unlikely(!JsonWordAllWs(w))) break;
        p += 8;
    }

    /* Pinpoint first non-ws */
    while (p < end) {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p++;
        } else {
            break;
        }
    }

    return p;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonCheckNoCtlInString --
 *
 *      Validate that a JSON string value contains no unescaped ASCII control
 *      characters below 0x20. When a control character is found, an error is
 *      recorded in the parse context so the caller can produce a useful
 *      parse error message.
 *
 * Results:
 *      NS_OK when no control character is present,
 *      NS_ERROR when an invalid control character is found.
 *
 * Side effects:
 *      On failure, updates the parser/error state (e.g., sets an error
 *      message and/or position information) according to the surrounding
 *      implementation conventions.
 *
 *----------------------------------------------------------------------
 */
static inline Ns_ReturnCode
JsonCheckNoCtlInString(JsonParser *jp, const unsigned char *p, const unsigned char *end)
{
    const unsigned char *bad = JsonFindCtlLt0x20(p, end);
    if (unlikely(bad != NULL)) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: unescaped control character in string",
                         (unsigned long)(bad - jp->start));
        return NS_ERROR;
    }
    return NS_OK;
}

/*======================================================================
 * Function Implementations: Parser cursor helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSkipWs --
 *
 *      Advance the parser cursor past JSON whitespace characters.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Updates jp->p to the first non-whitespace byte (or jp->end).
 *
 *----------------------------------------------------------------------
 */
static inline void
JsonSkipWs(JsonParser *jp)
{
    jp->p = JsonSkipWsPtr(jp->p, jp->end);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonPeek --
 *
 *      Return the next input byte without consuming it.
 *
 * Results:
 *      The next byte as an int in the range 0..255, or -1 on end of input.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
JsonPeek(JsonParser *jp)
{
    return (jp->p < jp->end) ? (int)(unsigned char)*jp->p : -1;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonGet --
 *
 *      Consume and return the next input byte.
 *
 * Results:
 *      The consumed byte as an int in the range 0..255, or -1 on end of input.
 *
 * Side Effects:
 *      Advances jp->p by one byte when not at end of input.
 *
 *----------------------------------------------------------------------
 */
static int
JsonGet(JsonParser *jp)
{
    return (jp->p < jp->end) ? (int)(unsigned char)*jp->p++ : -1;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonExpect --
 *
 *      Consume the next byte and verify that it matches the expected
 *      character.  On mismatch, set a parse error message including
 *      the byte offset.
 *
 * Results:
 *      NS_OK when the expected character was found, NS_ERROR otherwise.
 *
 * Side Effects:
 *      Advances jp->p by one byte. On error, appends a message to jp->errDsPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonExpect(JsonParser *jp, int ch, const char *what)
{
    Ns_ReturnCode result = NS_OK;
    int c = JsonGet(jp);

    if (c != ch) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: expected %s",
                         (unsigned long)((jp->p > jp->start ? (jp->p - 1) : jp->p) - jp->start),
                         what);
        result = NS_ERROR;
    }
    return result;
}


/*======================================================================
 * Function Implementations: Number and escape decoding helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonDecodeHex4 --
 *
 *      Decode exactly four hexadecimal digits from the input stream into a
 *      16-bit value. The function reads the next four bytes via JsonGet()
 *      and interprets them as a hexadecimal number (0000..FFFF). On invalid
 *      digits or premature end of input, a parse error message is appended to
 *      jp->errDsPtr.
 *
 * Results:
 *      NS_OK on success with *u16Ptr set to the decoded value.
 *      NS_ERROR on failure with an error message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p by four bytes on success (and by the number of bytes
 *      consumed before detecting an error on failure).
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonDecodeHex4(JsonParser *jp, uint16_t *u16Ptr)
{
    unsigned int v = 0u;
    int          i;

    NS_NONNULL_ASSERT(jp != NULL);
    NS_NONNULL_ASSERT(u16Ptr != NULL);

    for (i = 0; i < 4; i++) {
        int          c = JsonGet(jp);
        unsigned int d;

        if (c < 0) {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: incomplete unicode escape",
                             (unsigned long)(jp->p - jp->start));
            return NS_ERROR;
        }
        if (c >= '0' && c <= '9') {
            d = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = 10u + (unsigned int)(c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            d = 10u + (unsigned int)(c - 'A');
        } else {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: invalid unicode escape",
                             (unsigned long)((jp->p - 1) - jp->start));
            return NS_ERROR;
        }
        v = (v << 4) | d;
    }

    *u16Ptr = (uint16_t)v;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonDecodeUnicodeEscape --
 *
 *      Decode a JSON Unicode escape sequence starting at the current cursor
 *      position. This function is called after the backslash and 'u' have
 *      already been consumed. It decodes the following four hex digits and
 *      handles UTF-16 surrogate pairs as required by JSON:
 *
 *          - If the first code unit is a high surrogate (D800..DBFF),
 *            the function requires a following "\u" escape containing a
 *            low surrogate (DC00..DFFF) and combines both into a single
 *            Unicode code point in the range U+10000..U+10FFFF.
 *
 *          - If the first code unit is a low surrogate without a preceding
 *            high surrogate, the function reports an error.
 *
 * Results:
 *      NS_OK on success with *cpPtr set to the decoded Unicode code point.
 *      NS_ERROR on failure with an error message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the consumed hex digits and, for surrogate pairs,
 *      past the additional "\uXXXX" sequence.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonDecodeUnicodeEscape(JsonParser *jp, uint32_t *cpPtr)
{
    uint16_t u1;

    if (JsonDecodeHex4(jp, &u1) != NS_OK) {
        return NS_ERROR;
    }

    if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
        /* high surrogate: must be followed by \uXXXX low surrogate */
        int      c1 = JsonGet(jp);
        int      c2 = JsonGet(jp);
        uint16_t u2;

        if (c1 != '\\' || c2 != 'u') {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: missing low surrogate",
                             (unsigned long)((jp->p > jp->start ? (jp->p - 1) : jp->p) - jp->start));
            return NS_ERROR;
        }
        if (JsonDecodeHex4(jp, &u2) != NS_OK) {
            return NS_ERROR;
        }
        if (u2 < 0xDC00u || u2 > 0xDFFFu) {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: invalid low surrogate",
                             (unsigned long)((jp->p - 1) - jp->start));
            return NS_ERROR;
        }

        *cpPtr = 0x10000u + (((uint32_t)(u1 - 0xD800u)) << 10) + (uint32_t)(u2 - 0xDC00u);
        return NS_OK;

    } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: unexpected low surrogate",
                         (unsigned long)((jp->p - 1) - jp->start));
        return NS_ERROR;
    }

    *cpPtr = (uint32_t)u1;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonScanNumber --
 *
 *      Scan a JSON number starting at jp->p and advance jp->p to the
 *      first character after the number. The scanner validates JSON
 *      number grammar and reports errors via jp->errDsPtr.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Advances jp->p on success. On failure, jp->p is left at the
 *      position where scanning detected the problem (best effort).
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonScanNumber(JsonParser *jp, const unsigned char **startPtr,
               const unsigned char **endPtr, bool *sawFracOrExpPtr)
{
    const unsigned char *s, *p;
    bool                sawFracOrExp = NS_FALSE;

    NS_NONNULL_ASSERT(jp != NULL);
    NS_NONNULL_ASSERT(startPtr != NULL);
    NS_NONNULL_ASSERT(endPtr != NULL);
    NS_NONNULL_ASSERT(sawFracOrExpPtr != NULL);

    s = p = jp->p;
    *startPtr = s;

    /*
     * JSON number grammar (RFC 8259):
     *   number = [ minus ] int [ frac ] [ exp ]
     *   int    = zero / ( digit1-9 *digit )
     *   frac   = '.' 1*digit
     *   exp    = ('e'/'E') ['+'/'-'] 1*digit
     */

    if (*p == '-') {
        p++;
        if (p >= jp->end) {
            if (jp->errDsPtr != NULL) {
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: parse error at byte %lu: unexpected end in number",
                                 (unsigned long)(p - jp->start));
            }
            return NS_ERROR;
        }
        if (*p < '0' || *p > '9') {
            if (jp->errDsPtr != NULL) {
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: parse error at byte %lu: expected digit after '-'",
                                 (unsigned long)(p - jp->start));
            }
            return NS_ERROR;
        }
    }

    /* int */
    if (*p == '0') {
        p++;
        /* no leading zeros allowed if more digits follow */
        if (*p >= '0' && *p <= '9') {
            if (jp->errDsPtr != NULL) {
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: parse error at byte %lu: invalid number",
                                 (unsigned long)(p - jp->start));
            }
            return NS_ERROR;
        }
    } else if (*p >= '1' && *p <= '9') {
        do {
            p++;
        } while (*p >= '0' && *p <= '9');
    } else {
        if (jp->errDsPtr != NULL) {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: invalid number",
                             (unsigned long)(p - jp->start));
        }
        return NS_ERROR;
    }

    /* frac */
    if (*p == '.') {
        const unsigned char *dot = p;
        sawFracOrExp = NS_TRUE;
        p++;
        if (!(*p >= '0' && *p <= '9')) {
            if (jp->errDsPtr != NULL) {
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: parse error at byte %lu: invalid number",
                                 (unsigned long)(dot - jp->start));
            }
            return NS_ERROR;
        }
        do {
            p++;
        } while (*p >= '0' && *p <= '9');
    }

    /* exp */
    if (*p == 'e' || *p == 'E') {
        const unsigned char *e = p;
        sawFracOrExp = NS_TRUE;
        p++;
        if (*p == '+' || *p == '-') {
            p++;
        }
        if (!(*p >= '0' && *p <= '9')) {
            if (jp->errDsPtr != NULL) {
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: parse error at byte %lu: invalid number",
                                 (unsigned long)(e - jp->start));
            }
            return NS_ERROR;
        }
        do {
            p++;
        } while (*p >= '0' && *p <= '9');
    }

    jp->p = p;
    *endPtr = p;
    *sawFracOrExpPtr = sawFracOrExp;

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonNumberLexemeIsValid --
 *
 *      Check whether the byte sequence is a syntactically valid JSON number
 *      lexeme according to RFC 8259.
 *
 *      This function performs lexical validation only. It does not attempt
 *      numeric conversion and does not check for overflow, underflow, or
 *      range limits.
 *
 * Results:
 *      true if the sequence matches JSON number syntax.
 *      false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonNumberLexemeIsValid(const unsigned char *s, size_t len)
{
    JsonParser jp;
    const unsigned char *start, *end;
    bool sawFracOrExp;

    memset(&jp, 0, sizeof(jp));
    jp.p = s;
    jp.start = s;
    jp.end = s + len;
    jp.errDsPtr = NULL;  /* adjust JsonScanNumber to tolerate NULL, or give dummy */

    if (JsonScanNumber(&jp, &start, &end, &sawFracOrExp) != NS_OK) {
        return NS_FALSE;
    }
    return (jp.p == jp.end);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonValidateNumberString --
 *
 *      Validate that the byte sequence is a syntactically valid JSON number
 *      lexeme according to RFC 8259.
 *
 *      This helper is intended for JSON generation paths, where a Tcl value
 *      is emitted as a JSON number literal. Validation is purely lexical and
 *      does not perform numeric conversion.
 *
 * Results:
 *      NS_OK if the sequence is a valid JSON number.
 *      NS_ERROR otherwise.
 *
 * Side effects:
 *      On failure, appends an explanatory error message to errDsPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonValidateNumberString(const unsigned char *s, size_t len, Ns_DString *errDsPtr)
{
    JsonParser     jp;
    Ns_ReturnCode  status = NS_OK;
    bool           sawFracOrExp;
    const unsigned char *p;

    NS_NONNULL_ASSERT(s != NULL);
    NS_NONNULL_ASSERT(errDsPtr != NULL);

    /*
     * Init synthetic parser over the lexeme.
     */
    memset(&jp, 0, sizeof(jp));
    jp.p        = s;
    jp.start    = s;
    jp.end      = s + len;
    jp.errDsPtr = errDsPtr;

    /*
     * We need no jp.tmpDs and jp.keyTable, since these are just for "-output
     * set" and for string parsing (actully key sharing).
     */
    status = JsonScanNumber(&jp, &s, &p, &sawFracOrExp);
    if (status == NS_OK && jp.p != jp.end) {
        Ns_DStringPrintf(errDsPtr,
                         "ns_json: invalid number lexeme: trailing characters after byte %lu",
                         (unsigned long)(jp.p - jp.start));
        status = NS_ERROR;
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonNumberObjToLexeme --
 *
 *      Convert a Tcl object to a valid JSON number lexeme.
 *
 *      This helper is used for explicit numeric typing (e.g. "-type number")
 *      in commands such as "ns_json value" and "ns_json triples setvalue".
 *      The function attempts to interpret the supplied Tcl value as a JSON
 *      number and returns a Tcl object whose string representation is a
 *      valid JSON number lexeme.
 *
 *      The conversion follows these rules:
 *
 *        1. If the current string representation of the object is already
 *           a valid JSON number lexeme, it is preserved unchanged.
 *
 *        2. Otherwise, the function attempts to interpret the value as a
 *           Tcl numeric value (integer or double).  If successful, the
 *           normalized Tcl numeric representation is used.
 *
 *        3. The resulting string representation is validated again to
 *           ensure that it conforms to the JSON number grammar.
 *
 *      Values that cannot be interpreted as numeric Tcl values or whose
 *      normalized representation is not a valid JSON number cause an
 *      error to be reported.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      On success, *outObjPtr is set to a Tcl object whose string
 *      representation is a valid JSON number lexeme.  This may be the
 *      original object or a newly created normalized numeric object.
 *      On failure, an error message is left in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonNumberObjToLexeme(Tcl_Interp *interp, Tcl_Obj *valueObj, Tcl_Obj **outObjPtr)
{
    const char *s;
    TCL_SIZE_T len;
    Ns_DString errDs;

    NS_NONNULL_ASSERT(valueObj != NULL);
    NS_NONNULL_ASSERT(outObjPtr != NULL);

    Tcl_DStringInit(&errDs);
    s = Tcl_GetStringFromObj(valueObj, &len);
    if (JsonValidateNumberString((const unsigned char *)s, (size_t)len, &errDs) == NS_OK) {
        *outObjPtr = valueObj;
        Tcl_DStringFree(&errDs);
        return NS_OK;
    }
    Tcl_DStringSetLength(&errDs, 0);

    {
        Tcl_WideInt w;

        if (Tcl_GetWideIntFromObj(NULL, valueObj, &w) == TCL_OK) {
            Tcl_Obj *obj = Tcl_NewWideIntObj(w);

            s = Tcl_GetStringFromObj(obj, &len);
            if (JsonValidateNumberString((const unsigned char *)s, (size_t)len, &errDs) != NS_OK) {
                Tcl_DStringResult(interp, &errDs);
                return NS_ERROR;
            }
            *outObjPtr = obj;
            Tcl_DStringFree(&errDs);
            return NS_OK;
        }
    }

    {
        double d;

        if (Tcl_GetDoubleFromObj(NULL, valueObj, &d) == TCL_OK) {
            Tcl_Obj *obj = Tcl_NewDoubleObj(d);

            s = Tcl_GetStringFromObj(obj, &len);
            if (JsonValidateNumberString((const unsigned char *)s, (size_t)len, &errDs) != NS_OK) {
                Tcl_DStringResult(interp, &errDs);
                return NS_ERROR;
            }
            *outObjPtr = obj;
            Tcl_DStringFree(&errDs);
            return NS_OK;
        }
    }

    Ns_TclPrintfResult(interp, "expected numeric Tcl value, got \"%s\"", s);
    Tcl_DStringFree(&errDs);

    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonCheckStringSpan --
 *
 *      Validate a plain (already decoded) substring of a JSON string and
 *      enforce the configured maximum decoded string length.
 *
 *      The span [p,q) is expected to contain only unescaped characters from
 *      the JSON input. The function checks that the span contains no
 *      disallowed ASCII control characters and that appending the span would
 *      not exceed the configured maxString limit.
 *
 * Results:
 *      NS_OK when the span is valid and within bounds.
 *      NS_ERROR when an invalid control character is found or the maximum
 *      decoded string length would be exceeded.
 *
 * Side effects:
 *      On failure, appends an explanatory error message to jp->errDsPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonCheckStringSpan(JsonParser *jp, const unsigned char *p, const unsigned char *q, size_t outLen)
{
    size_t add;

    NS_NONNULL_ASSERT(jp != NULL);
    NS_NONNULL_ASSERT(p != NULL);
    NS_NONNULL_ASSERT(q != NULL);
    NS_NONNULL_ASSERT(q >= p);

    add = (size_t)(q - p);

    if (add > 0u && JsonCheckNoCtlInString(jp, p, q) != NS_OK) {
        return NS_ERROR;
    }

    if (jp->opt->maxString > 0u && outLen + add > (size_t)jp->opt->maxString) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: string too long (max %lu)",
                         (unsigned long)(p - jp->start),
                         (unsigned long)jp->opt->maxString);
        return NS_ERROR;
    }

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonValidateValue --
 *
 *      Validate and optionally normalize a value according to a requested
 *      JSON value type.
 *
 *      For scalar types, this function checks that inObj is compatible with
 *      the requested type and, when applicable, produces a canonicalized
 *      representation in *outObjPtr (e.g., canonical boolean objects and the
 *      dedicated JSON-null Tcl object).  For numbers, the function validates
 *      the JSON number lexeme.  For container types (object/array), it
 *      validates that inObj is a plausible triples list (length multiple of 3)
 *      and may perform additional checks depending on the configuration
 *      (e.g., number validation within the container).
 *
 *      JSON null values are represented by a dedicated Tcl object type
 *      (JsonNullObjType).  When normalization for JSON_VT_NULL is requested,
 *      the value is replaced by the canonical null object stored in the
 *      JSON atom table.
 *
 *      The what argument is included in generated error messages to provide
 *      context (e.g., "triples setvalue").
 *
 * Results:
 *      NS_OK when the input is valid for the requested type, NS_ERROR
 *      otherwise.
 *
 * Side effects:
 *      On success, *outObjPtr may be set to a normalized Tcl object that the
 *      caller should use in place of inObj.  On error, leaves an explanatory
 *      message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonValidateValue(Tcl_Interp *interp, JsonValueType vt, Tcl_Obj *inObj, Tcl_Obj **outObjPtr,
                  const char *what)
{
    Tcl_Obj *outObj = inObj;

    switch (vt) {

    case JSON_VT_OBJECT:
    case JSON_VT_ARRAY:
        if (JsonTriplesRequireValidContainerObj(interp, inObj, NS_TRUE, NS_TRUE, what) != NS_OK) {
            return NS_ERROR;
        }
        break;

    case JSON_VT_NUMBER:
        if (JsonNumberObjToLexeme(interp, inObj, outObjPtr) != NS_OK) {
            return NS_ERROR;
        }
        break;

    case JSON_VT_BOOL: {
        int isTrue = 0;

        if (Tcl_GetBooleanFromObj(interp, inObj, &isTrue) != TCL_OK) {
            return NS_ERROR;
        }

        /*
         * Prefer canonical JSON atoms when available (true/false), otherwise
         * fall back to a boolean object.
         */
        outObj = JsonAtomObjs[isTrue ? JSON_ATOM_TRUE : JSON_ATOM_FALSE];
        break;

    }

    case JSON_VT_NULL:
        /*
         * Normalize to the canonical JSON null object.
         */
        outObj = JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING];
        break;

    case JSON_VT_STRING:
        /* no validation */
        break;

    case JSON_VT_AUTO:
    default:
        Ns_TclPrintfResult(interp, "ns_json %s: unsupported type", what);
        return NS_ERROR;
    }

    *outObjPtr = outObj;
    return NS_OK;
}


/*======================================================================
 * Function Implementations: Key sharing/interning helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonAllocEntryProc --
 *
 *      Tcl hash table allocation hook for the custom JsonKey hash key type.
 *      This function is called by Tcl when a new hash entry has to be created.
 *
 *      The incoming keyPtr points to a transient probe key (JsonKey with
 *      bytes/len referring into the JSON input buffer).  This function
 *      allocates a persistent JsonKey and stores its pointer in the hash
 *      entry's one-word key field so the key remains valid for the lifetime
 *      of the hash entry.
 *
 *      The entry's hash value (clientData) is initialized to NULL; callers
 *      typically store an interned Tcl_Obj* later via Tcl_SetHashValue().
 *
 * Results:
 *      Returns a freshly allocated Tcl_HashEntry* with a persistent key.
 *
 * Side Effects:
 *      Allocates memory for the Tcl_HashEntry and the persistent JsonKey.
 *
 *----------------------------------------------------------------------
 */
static Tcl_HashEntry *
JsonAllocEntryProc(Tcl_HashTable *tablePtr, void *keyPtr)
{
    const JsonKey *probe = (const JsonKey *)keyPtr;
    JsonKey     *stored;
    Tcl_HashEntry *hPtr;

    stored = JsonKeyAlloc(NULL, probe->bytes, probe->len);

    hPtr = ns_malloc(sizeof(Tcl_HashEntry));
    hPtr->tablePtr = tablePtr;
    hPtr->clientData = 0;
    hPtr->key.oneWordValue = (char *)stored;

    return hPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonFreeEntryProc --
 *
 *      Tcl hash table free hook for the custom JsonKey hash key type.
 *      This function is called by Tcl when an entry is deleted or the hash
 *      table is destroyed.
 *
 *      The function releases:
 *
 *        - The hash value (if set): expected to be an interned Tcl_Obj*
 *          stored via Tcl_SetHashValue().  The value is reference counted,
 *          therefore it is decremented here.
 *
 *        - The persistent JsonKey stored in the entry key.oneWordValue.
 *
 *        - The Tcl_HashEntry itself (because entries are allocated by
 *          JsonAllocEntryProc using ns_malloc()).
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Decrements the refcount of the stored Tcl_Obj value (if any) and
 *      frees memory for the JsonKey and the hash entry.
 *
 *----------------------------------------------------------------------
 */
static void
JsonFreeEntryProc(Tcl_HashEntry *hPtr)
{
    JsonKey *stored = (JsonKey *)(void *)hPtr->key.oneWordValue;
    Tcl_Obj *o = (Tcl_Obj *)Tcl_GetHashValue(hPtr);
    if (o != NULL) {
        Tcl_DecrRefCount(o);
    }
    ns_free(stored);
    ns_free(hPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyAlloc --
 *
 *      Allocate a stable, NUL-terminated key buffer suitable for use as
 *      a hash-table key in the JSON key-sharing table.
 *
 *      The function copies exactly keyLen bytes from keyString and appends
 *      a trailing '\0' to ensure that all hash/compare paths observe the
 *      same canonical byte sequence.
 *
 * Results:
 *      Pointer to the newly allocated, NUL-terminated key buffer.
 *
 * Side effects:
 *      Allocates memory for the returned key. The caller is responsible
 *      for releasing it (directly or indirectly via the table cleanup
 *      mechanism) according to the surrounding design.
 *
 *----------------------------------------------------------------------
 */
static JsonKey *
JsonKeyAlloc(JsonParser *UNUSED(jp), const char *bytes, TCL_SIZE_T len)
{
    JsonKey *k;
    char    *dst;

    k = (JsonKey *)ns_malloc(sizeof(JsonKey) + (size_t)len + 1u);
    //jp->nKeyAlloc++;

    dst = (char *)(k + 1);
    memcpy(dst, bytes, (size_t)len);
    dst[len] = '\0';

    k->len   = len;
    k->bytes = dst;

    return k;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonCompareKeysProc --
 *
 *      Compare two JSON-interned key values for equality as required by
 *      the Tcl_HashTable custom key type.
 *
 *      The comparison must be consistent with JsonHashKeyProc(), i.e.,
 *      keys that compare equal must yield identical hash values.
 *
 * Results:
 *      Standard Tcl hash key compare result:
 *        - returns 0 when the keys are equal
 *        - returns nonzero when the keys differ
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
JsonCompareKeysProc(void *keyPtr, Tcl_HashEntry *hPtr)
{
    const JsonKey *k1 = (const JsonKey *)keyPtr;
    const JsonKey *k2 = (const JsonKey *)Tcl_GetHashKey(hPtr->tablePtr, hPtr);

    if (k1->len != k2->len) {
        return 0;
    }
    return (memcmp(k1->bytes, k2->bytes, (size_t)k1->len) == 0);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonHashKeyProc --
 *
 *      Compute the hash value for a JSON key as required by the
 *      Tcl_HashTable custom key type.
 *
 *      The hash must be computed over the same canonical key bytes used by
 *      JsonCompareKeysProc() (including the exact length semantics), so
 *      that hash/compare behavior is consistent and lookups are reliable.
 *
 *      The hash function is the same, which is used in Tcl.
 *
 * Results:
 *      Hash value for the provided key suitable for Tcl_HashTable indexing.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static TCL_HASH_TYPE
JsonHashKeyProc(Tcl_HashTable *UNUSED(tablePtr), void *keyPtr)
{
    const JsonKey       *k = (const JsonKey *)keyPtr;
    const unsigned char *bytes = (const unsigned char *)k->bytes;
    TCL_SIZE_T           cnt = k->len;
    unsigned int         hashValue = 0u;

    while (cnt-- > 0) {
        hashValue += (hashValue << 3) + bytes[0];
        bytes++;
    }
    return hashValue;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonInternKeyObj --
 *
 *      Intern a JSON object member name and return it as a Tcl_Obj.
 *
 *      The key is looked up (and optionally created) in the JSON key
 *      sharing table, returning a stable canonical key pointer. A Tcl_Obj
 *      is then created (or reused) from that canonical representation, to
 *      reduce duplicate allocations and refcount churn for repeated keys.
 *
 * Results:
 *      Tcl_Obj* representing the interned key. The returned object is ready
 *      for use by the caller; ownership/refcounting follows normal Tcl
 *      conventions for returned objects in this subsystem.
 *
 * Side effects:
 *      May allocate and insert a new key into the key table and may allocate
 *      a new Tcl_Obj for the key (or obtain one from an internal cache),
 *      depending on the implementation strategy.
 *
 *----------------------------------------------------------------------
 */
static inline Tcl_Obj *
JsonInternKeyObj(JsonParser *jp, const char *bytes, TCL_SIZE_T len)
{
    Tcl_HashEntry *hPtr;
    int            isNew;
    JsonKey        probe;

    probe.bytes = bytes;
    probe.len   = len;

    hPtr = Tcl_CreateHashEntry(&jp->keyTable, (const char *)&probe, &isNew);
    if (isNew) {
        Tcl_Obj *o = Tcl_NewStringObj(bytes, len);

        Tcl_IncrRefCount(o);
        Tcl_SetHashValue(hPtr, o);
        jp->nKeyObjIncr++;
        return o;
    }
    jp->nKeyReuse++;

    return (Tcl_Obj *)Tcl_GetHashValue(hPtr);
}

/*======================================================================
 * Function Implementations: Type detection and triples/container validation.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonTypeString --
 *
 *      Return the canonical string representation of a JsonValueType.
 *
 *      The function maps the internal JsonValueType enumeration to the
 *      corresponding atomized JSON type name (e.g., "string", "number",
 *      "object").  The returned string is taken from the atom table
 *      (JsonAtomObjs[]) and is therefore stable and shared.
 *
 * Results:
 *      Returns a pointer to the constant string representation of the
 *      specified JSON value type.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
JsonTypeString(JsonValueType vt)
{
    return Tcl_GetString(JsonAtomObjs[vt]);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTypeObjToVt --
 *
 *      Map a type token Tcl object to the corresponding JsonValueType.
 *
 *      The type token is expected to name a JSON value type such as
 *      "string", "number", "boolean", "null", "object", or "array".
 *      The returned value is used to drive validation and emission of
 *      values stored in the triples representation.
 *
 * Results:
 *      JsonValueType value corresponding to the provided type token.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static JsonValueType
JsonTypeObjToVt(Tcl_Obj *typeObj)
{
    /* Fast path: atom identity checks. */
    if (typeObj == JsonAtomObjs[JSON_ATOM_T_STRING])   { return JSON_VT_STRING; }
    if (typeObj == JsonAtomObjs[JSON_ATOM_T_NUMBER])   { return JSON_VT_NUMBER; }
    if (typeObj == JsonAtomObjs[JSON_ATOM_T_BOOLEAN])  { return JSON_VT_BOOL;   }
    if (typeObj == JsonAtomObjs[JSON_ATOM_T_NULL])     { return JSON_VT_NULL;   }
    if (typeObj == JsonAtomObjs[JSON_ATOM_T_OBJECT])   { return JSON_VT_OBJECT; }
    if (typeObj == JsonAtomObjs[JSON_ATOM_T_ARRAY])    { return JSON_VT_ARRAY;  }

    {
        TCL_SIZE_T  tlen;
        const char *t = Tcl_GetStringFromObj(typeObj, &tlen);

        if (tlen == 6 && memcmp(t, "string", 6) == 0)   { return JSON_VT_STRING; }
        if (tlen == 6 && memcmp(t, "number", 6) == 0)   { return JSON_VT_NUMBER; }
        if (tlen == 7 && memcmp(t, "boolean", 7) == 0)  { return JSON_VT_BOOL;   }
        if (tlen == 4 && memcmp(t, "bool", 4) == 0)     { return JSON_VT_BOOL;   }
        if (tlen == 4 && memcmp(t, "null", 4) == 0)     { return JSON_VT_NULL;   }
        if (tlen == 6 && memcmp(t, "object", 6) == 0)   { return JSON_VT_OBJECT; }
        if (tlen == 5 && memcmp(t, "array", 5) == 0)    { return JSON_VT_ARRAY;  }
    }
    return JSON_VT_AUTO;
}


/*
 *----------------------------------------------------------------------
 *
 * JsonObjIsAtom --
 *
 *      Test whether a Tcl object corresponds to a specific JSON atom.
 *
 *      This helper compares a Tcl object with one of the predefined
 *      JSON keyword atoms (e.g., "type", "properties", "items").
 *      The comparison is optimized for atomized objects by first
 *      checking pointer identity.  If the object is not the same
 *      instance as the atom object, the function falls back to
 *      comparing the string representations.
 *
 *      This allows callers to efficiently test schema dictionary
 *      keys against known JSON keywords while still accepting
 *      non-atomized input originating from user-provided schemas.
 *
 * Results:
 *      true if the Tcl object represents the specified atom.
 *      false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static inline bool
JsonObjIsAtom(Tcl_Obj *obj, JsonAtom atom)
{
    Tcl_Obj *atomObj = JsonAtomObjs[atom];

    /* Fast path: pointer identity (most common case). */
    if (atomObj == obj) {
        return NS_TRUE;
    }

    /* Slow path: compare string representations. */
    {
        TCL_SIZE_T slen = 0, olen = 0;
        const char *s = Tcl_GetStringFromObj(atomObj, &slen);
        const char *o = Tcl_GetStringFromObj(obj, &olen);

        return (slen == olen && memcmp(o, s, (size_t)olen) == 0);
    }
}

/*
 *-----------------------------------------------------------------------
 *
 * JsonValueTypeDetect --
 *
 *      Determine the JSON value type for valueObj in the same conservative
 *      manner as ns_json value -type auto.
 *
 *      This function is a pure classifier: it never reports an error and
 *      never sets the interpreter result. The final fallback is always
 *      JSON_VT_STRING.
 *
 *      Container detection is performed first using
 *      JsonTriplesDetectContainerType(). A value is only classified as a
 *      container (object/array) when it is a plausible triples list (length
 *      multiple of 3). Otherwise, container markers are ignored and the value
 *      is classified as a scalar.
 *
 *      Number validation is not performed here. If the classifier selects
 *      JSON_VT_NUMBER, the number lexeme must be validated at emission time
 *      (e.g. in the JSON_VT_NUMBER output branch), regardless of whether the
 *      type was chosen explicitly or via AUTO.
 *
 * Results:
 *      The inferred JsonValueType.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------
 */
static JsonValueType
JsonValueTypeDetect(Tcl_Interp *interp, Tcl_Obj *valueObj)
{
    Tcl_Obj      *rootValue = NULL;
    JsonValueType tvt;

    tvt = TriplesDetectRootWrapper(interp, valueObj, NULL, &rootValue);
    if (tvt != JSON_VT_AUTO) {
        /* Root wrapper present: its TYPE wins. */
        return tvt;
    }

    /*
     * Conservative scalar AUTO: number, boolean, null atom, else string.
     */
    {
        int         b;
        TCL_SIZE_T  len;
        const char *s = Tcl_GetStringFromObj(valueObj, &len);

        if (JsonNumberLexemeIsValid((const unsigned char *)s, (size_t)len)) {
            return JSON_VT_NUMBER;
        }
        if (!(s[0] == '0' && len > 1)
            && Tcl_GetBooleanFromObj(NULL, valueObj, &b) == TCL_OK) {
            return JSON_VT_BOOL;
        }
        if (JsonIsNullObj(valueObj)) {
            return JSON_VT_NULL;
        }
    }

    return JSON_VT_STRING;
}

/*
 * ----------------------------------------------------------------------
 *
 * TriplesDetectRootWrapper --
 *
 *      Return the value type based on the a root element. For wrapper it is
 *      of the form: {"" TYPE VALUE}
 *
 *      When present, return the root type (object/array/string/number/boolean/null)
 *      and optionally the wrapped root value.
 *
 *      This function is intentionally strict: it recognizes only the wrapper
 *      with an empty key (JSON_ATOM_EMPTY) and a valid JSON type token.
 *
 * Results:
 *      Detected JsonValueType (never JSON_VT_AUTO). On no wrapper, returns
 *      JSON_VT_AUTO.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static JsonValueType
TriplesDetectRootWrapper(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj **rootTypeObjPtr, Tcl_Obj **rootValuePtr)
{
    Tcl_Obj     **lv;
    TCL_SIZE_T    lc, klen;
    JsonValueType vt;

    *rootValuePtr = NULL;
    if (rootTypeObjPtr != NULL) {
        *rootTypeObjPtr = NULL;
    }

    /*
     * Check for a typed null object before converting triplesObj to a list.
     * Such a conversion would shimmer the Tcl_Obj and break null-object
     * detection.  Similar fast-path checks could be added later for other
     * relevant Tcl object types.
     */
    if (JsonIsNullObj(triplesObj)) {
        return JSON_VT_AUTO;
    }

#if 0
    {
        TCL_SIZE_T len;
        (void)Tcl_GetStringFromObj(triplesObj, &len);
        fprintf(stderr, "DEBUG TriplesDetectRootWrapper length %d type %s\n",
                len, triplesObj->typePtr ? triplesObj->typePtr->name : "NONE");
    }
#endif
    if (Tcl_ListObjGetElements(interp, triplesObj, &lc, &lv) != TCL_OK) {
        return JSON_VT_AUTO;
    }

    if (lc != 3) {
        return JSON_VT_AUTO;
    }

    /*
     * Root wrapper key is the empty string.
     */
    (void) Tcl_GetStringFromObj(lv[0], &klen);
    if (klen != 0) {
        return JSON_VT_AUTO;
    }

    vt = JsonTypeObjToVt(lv[1]);
    if (vt == JSON_VT_AUTO) {
        return JSON_VT_AUTO;
    }

    *rootValuePtr = lv[2];
    if (rootTypeObjPtr != NULL) {
        *rootTypeObjPtr = lv[1];
    }

    return vt;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonRequireValidNumberObj --
 *
 *      Validate that the provided Tcl object contains a valid JSON number
 *      lexeme.
 *
 *      The input is interpreted as a JSON number token (not as a Tcl number).
 *      The function rejects lexemes that are not valid per JSON syntax
 *      (e.g., leading zeros such as "01", or non-numeric tokens such as "NaN").
 *
 * Results:
 *      NS_OK when valueObj contains a valid JSON number lexeme, NS_ERROR
 *      otherwise.
 *
 * Side effects:
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonRequireValidNumberObj(Tcl_Interp *interp, Tcl_Obj *valueObj)
{
    Tcl_DString errDs;
    TCL_SIZE_T  len;
    const char *s = Tcl_GetStringFromObj(valueObj, &len);

    Tcl_DStringInit(&errDs);
    if (JsonValidateNumberString((const unsigned char *)s, (size_t)len, &errDs) != NS_OK) {
        if (interp != NULL) {
            Tcl_DStringResult(interp, &errDs);
        } else {
            Tcl_DStringFree(&errDs);
        }
        return NS_ERROR;
    }
    Tcl_DStringFree(&errDs);
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesRequireValidContainerObj --
 *
 *      Validate that a Tcl object contains plausible triples for a JSON
 *      container value (object or array).
 *
 *      The function requires that containerObj is a Tcl list whose length
 *      is a multiple of 3 (key-or-index, type, value).  When allowEmpty is
 *      false, an empty list is rejected.  When validateNumbers is true,
 *      the function additionally validates all number lexemes tagged with
 *      type "number" within the container, recursing into nested object/array
 *      triples.
 *
 *      The what argument is included in generated error messages to provide
 *      context (e.g., "triples setvalue", "value").
 *
 * Results:
 *      NS_OK when the container triples are valid, NS_ERROR otherwise.
 *
 * Side effects:
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonTriplesRequireValidContainerObj(Tcl_Interp *interp, Tcl_Obj *containerObj,
                                   bool allowEmpty, bool validateNumbers,
                                   const char *what)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    if (Tcl_ListObjGetElements(interp, containerObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((!allowEmpty && lc == 0) || (lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
            "ns_json %s: triples length must be multiple of 3%s",
            what,
            allowEmpty ? "" : " and non-empty");
        return NS_ERROR;
    }

    if (validateNumbers) {
        for (TCL_SIZE_T i = 0; i < lc; i += 3) {
            Tcl_Obj      *typeObj      = lv[i+1];
            Tcl_Obj      *elemValueObj = lv[i+2];
            JsonValueType vt           = JsonTypeObjToVt(typeObj);

            switch (vt) {
            case JSON_VT_OBJECT:
            case JSON_VT_ARRAY:
                if (JsonTriplesRequireValidContainerObj(interp, elemValueObj,
                        NS_TRUE, validateNumbers, what) != NS_OK) {
                    return NS_ERROR;
                }
                break;

            case JSON_VT_NUMBER:
                if (JsonRequireValidNumberObj(interp, elemValueObj) != NS_OK) {
                    return NS_ERROR;
                }
                break;

            case JSON_VT_STRING:
            case JSON_VT_BOOL:
            case JSON_VT_NULL:
                /* no validation here */
                break;

            case JSON_VT_AUTO:
            default:
                Ns_TclPrintfResult(interp,
                    "ns_json %s: invalid triple type: %s",
                    what, Tcl_GetString(typeObj));
                return NS_ERROR;
            }
        }
    }

    return NS_OK;
}

/*======================================================================
 * Function Implementations: Triple appending.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesAppend --
 *
 *      Append one NAME TYPE VALUE triple to the given Tcl list.
 *
 *      This helper is used to build the triples representation for JSON
 *      containers by extending listObj with exactly three elements.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Extends listObj by three elements.
 *
 *----------------------------------------------------------------------
 */
static inline void
JsonTriplesAppend(Tcl_Obj *triplesObj,
                  Tcl_Obj *nameObj, Tcl_Obj *typeObj, Tcl_Obj *valueObj)
{
    (void) Tcl_ListObjAppendElement(NULL, triplesObj, nameObj);
    (void) Tcl_ListObjAppendElement(NULL, triplesObj, typeObj);
    (void) Tcl_ListObjAppendElement(NULL, triplesObj, valueObj);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesAppendVt --
 *
 *      Append one NAME TYPE VALUE triple to the given Tcl list, where
 *      the TYPE element is derived from the provided JsonValueType.
 *
 *      This helper is a convenience wrapper around JsonTriplesAppend()
 *      for callers that track the JSON value type as an enum rather than
 *      as a Tcl object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Extends triplesObj by three elements.
 *
 *----------------------------------------------------------------------
 */
static inline void
JsonTriplesAppendVt(Tcl_Obj *triplesObj, Tcl_Obj *nameObj, JsonValueType vt, Tcl_Obj *valueObj)
{
    JsonTriplesAppend(triplesObj, nameObj, JsonAtomObjs[vt], valueObj);
}


/*======================================================================
 * Function Implementations: Triples/path lookup and update helpers.
 *======================================================================
 */

/*
 * ----------------------------------------------------------------------
 *
 * TriplesLookupPath --
 *
 *      Locate an element in a triples structure and optionally return the
 *      stored value, stored type token, value type enum, and/or index paths
 *      suitable for use with lindex/lset.
 *
 *      The provided pathObj is a Tcl list of path segments.  The function
 *      walks the triples structure starting at triplesObj, resolving each
 *      segment with TriplesFind().  Descent into nested triples is permitted
 *      only when the stored type is object or array; attempting to descend
 *      into a scalar results in an error.
 *
 *      An empty path list addresses the root value (i.e., the whole triples
 *      document).  In this case, the returned value is triplesObj itself and
 *      the returned index paths are empty Tcl lists.  An empty index path is
 *      suitable for replacing the whole document via lset (e.g., "lset t {} v").
 *
 *      When requested for non-empty paths, valueIndexPathPtr and typeIndexPathPtr
 *      are set to Tcl index paths that address, respectively, the leaf value
 *      slot (base+2) and the leaf type slot (base+1) within the original
 *      triples list.
 *
 * Results:
 *      NS_OK on success, NS_ERROR if the path cannot be resolved or the
 *      triples structure is malformed.
 *
 * Side effects:
 *      On success, sets the requested output pointers.  On error, leaves an
 *      explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
TriplesLookupPath(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *triplesObj,
                  Tcl_Obj **valuePtr, Tcl_Obj **typePtr, JsonValueType *vtPtr,
                  Tcl_Obj **valueIndexPathPtr, Tcl_Obj **typeIndexPathPtr)
{
    Tcl_Obj   **pv;
    TCL_SIZE_T  pc;

    Tcl_Obj   *curTriples = triplesObj;
    Tcl_Obj   *vIndexPath = NULL;
    Tcl_Obj   *tIndexPath = NULL;

    /* root wrapper detection */
    Tcl_Obj      *rootValue = NULL;
    Tcl_Obj      *rootType  = NULL;
    JsonValueType rootVt    = JSON_VT_AUTO;
    bool          isWrapped = NS_FALSE;

    if (valuePtr != NULL)          { *valuePtr = NULL; }
    if (typePtr != NULL)           { *typePtr = NULL; }
    if (vtPtr != NULL)             { *vtPtr = JSON_VT_AUTO; }
    if (valueIndexPathPtr != NULL) { *valueIndexPathPtr = NULL; }
    if (typeIndexPathPtr != NULL)  { *typeIndexPathPtr = NULL; }

    if (Tcl_ListObjGetElements(interp, pathObj, &pc, &pv) != TCL_OK) {
        return NS_ERROR;
    }

    /*
     * Detect canonical root wrapper: "" TYPE VALUE
     */
    rootVt    = TriplesDetectRootWrapper(interp, triplesObj, &rootType, &rootValue);
    isWrapped = (rootVt != JSON_VT_AUTO);

    /*
     * Empty path addresses the root value (whole document).
     *
     * Note: callers that want to preserve the wrapper (e.g. getvalue -output triples)
     * should special-case pc==0 in the command and return triplesObj.
     */
    if (pc == 0) {
        if (isWrapped) {
            if (valuePtr != NULL) { *valuePtr = rootValue; }
            if (typePtr != NULL)  { *typePtr  = rootType; }
            if (vtPtr != NULL)    { *vtPtr    = rootVt; }
        } else {
            Ns_TclPrintfResult(interp, "ns_json triples: empty path requires root wrapper {\"\" TYPE VALUE}");
            goto err;
        }

        if (valueIndexPathPtr != NULL) {
            *valueIndexPathPtr = Tcl_NewListObj(0, NULL);
        }
        if (typeIndexPathPtr != NULL) {
            *typeIndexPathPtr = Tcl_NewListObj(0, NULL);
        }

        return NS_OK;
    }

    /*
     * Non-empty path: when wrapped, apply path to the wrapped VALUE.
     */
    if (isWrapped) {
        if (rootVt != JSON_VT_OBJECT && rootVt != JSON_VT_ARRAY) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples: cannot descend into %s",
                               Tcl_GetString(rootType));
            return NS_ERROR;
        }
        curTriples = rootValue;
    }

    /*
     * Build index paths. When wrapped, prefix "2" so the returned index path
     * applies to the original wrapped triples object.
     */
    vIndexPath = Tcl_NewListObj(0, NULL);
    tIndexPath = Tcl_NewListObj(0, NULL);

    if (isWrapped) {
        (void)Tcl_ListObjAppendElement(interp, vIndexPath, Tcl_NewIntObj(2));
        (void)Tcl_ListObjAppendElement(interp, tIndexPath, Tcl_NewIntObj(2));
    }

    for (TCL_SIZE_T pi = 0; pi < pc; pi++) {
        TCL_SIZE_T    base, lc;
        Tcl_Obj     **lv;
        JsonValueType vt;

        if (TriplesFind(interp, curTriples, pv[pi], &base) != NS_OK) {
            goto err;
        }

        if (Tcl_ListObjGetElements(interp, curTriples, &lc, &lv) != TCL_OK) {
            goto err;
        }

        if (pi == pc - 1) {
            if (valuePtr != NULL) { *valuePtr = lv[base + 2]; }
            if (typePtr != NULL)  { *typePtr  = lv[base + 1]; }
            if (vtPtr != NULL)    { *vtPtr    = JsonTypeObjToVt(lv[base + 1]); }

            (void)Tcl_ListObjAppendElement(interp, vIndexPath, Tcl_NewIntObj((int)(base + 2)));
            (void)Tcl_ListObjAppendElement(interp, tIndexPath, Tcl_NewIntObj((int)(base + 1)));

            if (valueIndexPathPtr != NULL) {
                *valueIndexPathPtr = vIndexPath;
            } else {
                Tcl_DecrRefCount(vIndexPath);
            }
            if (typeIndexPathPtr != NULL) {
                *typeIndexPathPtr = tIndexPath;
            } else {
                Tcl_DecrRefCount(tIndexPath);
            }

            return NS_OK;
        }

        vt = JsonTypeObjToVt(lv[base + 1]);
        if (vt != JSON_VT_OBJECT && vt != JSON_VT_ARRAY) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples: cannot descend into %s at path element %s",
                               Tcl_GetString(lv[base + 1]),
                               Tcl_GetString(pv[pi]));
            goto err;
        }

        (void)Tcl_ListObjAppendElement(interp, vIndexPath, Tcl_NewIntObj((int)(base + 2)));
        (void)Tcl_ListObjAppendElement(interp, tIndexPath, Tcl_NewIntObj((int)(base + 2)));

        curTriples = lv[base + 2];
    }

err:
    if (vIndexPath != NULL) Tcl_DecrRefCount(vIndexPath);
    if (tIndexPath != NULL) Tcl_DecrRefCount(tIndexPath);
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TripleKeyMatches --
 *
 *      Compare a triples key object with a path segment object.
 *
 *      The triples representation uses a "key-or-index" field for each
 *      element.  This helper determines whether a stored key matches the
 *      requested path segment, handling the object-key vs. array-index
 *      comparison rules used by TriplesFind().
 *
 * Results:
 *      NS_TRUE if keyObj matches segObj, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
TripleKeyMatches(Tcl_Obj *keyObj, Tcl_Obj *segObj)
{

    TCL_SIZE_T  klen, slen;
    const char *k = Tcl_GetStringFromObj(keyObj, &klen);
    const char *s = Tcl_GetStringFromObj(segObj, &slen);

    if (slen == 1 && s[0] == '*') return NS_TRUE;
    return (klen == slen && memcmp(k, s, (size_t)klen) == 0);
}

/*
 *----------------------------------------------------------------------
 *
 * TriplesFind --
 *
 *      Locate a key or index within a triples list and return the base
 *      index of the matching triple.
 *
 *      triplesObj must be a Tcl list in triples format:
 *          key-or-index type value ...
 *      The segObj argument identifies the requested element (object key or
 *      array index).  On success, *tripleBaseIdxPtr is set to the list index
 *      of the matching triple's key-or-index field.  The corresponding type
 *      and value are at base+1 and base+2.
 *
 * Results:
 *      NS_OK on success, NS_ERROR if the element cannot be found or the
 *      triples list is malformed.
 *
 * Side effects:
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
TriplesFind(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *segObj, TCL_SIZE_T *tripleBaseIdxPtr)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    if (Tcl_ListObjGetElements(interp, triplesObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if (lc == 0 || (lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
            "ns_json triples: input must be a list of {key|index type value} elements");
        return NS_ERROR;
    }

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        if (TripleKeyMatches(lv[i], segObj)) {
            *tripleBaseIdxPtr = i;
            return NS_OK;
        }
    }

    Ns_TclPrintfResult(interp, "ns_json triples: no such element: %s",
                       Tcl_GetString(segObj));
    return NS_ERROR;
}

/*----------------------------------------------------------------------
 *
 * JsonPointerToPathObj --
 *
 *      Convert an RFC 6901 JSON Pointer string into a Tcl path list
 *      suitable for triples navigation.
 *
 *      The JSON Pointer is split into reference tokens, with "~1" and "~0"
 *      unescaped to "/" and "~" respectively.  The result is a Tcl list
 *      where each element is a decoded path segment used by TriplesFind().
 *
 *      In addition to plain JSON Pointer strings ("" or "/..."), this
 *      function accepts the fragment form used in specifications and
 *      Problem Details error messages:
 *
 *          "#"        => whole document (map to empty path list)
 *          "#/a/b"    => same as "/a/b"
 *
 *      The leading '#' is treated only as the fragment marker.  URI
 *      percent-decoding of fragments is intentionally not performed.
 *
 * Results:
 *      Tcl path list object on success, NULL on error.
 *
 * Side effects:
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------*/
static Tcl_Obj *
JsonPointerToPathObj(Tcl_Interp *interp, const char *p, TCL_SIZE_T len)
{
    Tcl_DString listDs, segDs;
    TCL_SIZE_T  i, start;
    Tcl_Obj     *pathObj = NULL;
    const char  *origP = p;

    if (len < 0) {
        len = (TCL_SIZE_T)strlen(p);
    }

   /*
     * RFC 6901 JSON Pointer:
     * ""      => whole document (empty path list)
     * "/a/b"  => segments a, b
     *
     * Also accept fragment form:
     * "#"     => whole document
     * "#/a/b" => same as "/a/b"
     */
    if (len == 0) {
        return Tcl_NewListObj(0, NULL);
    }

    if (p[0] == '#') {
        if (len == 1) {
            return Tcl_NewListObj(0, NULL);
        }
        if (p[1] == '/') {
            p++;
            len--;
        } else {
            Ns_TclPrintfResult(interp,
                               "ns_json triples: invalid JSON pointer fragment (expected \"#\" or \"#/...\"): %s",
                               origP);
            return NULL;
        }
    }
    if (p[0] != '/') {
        Ns_TclPrintfResult(interp,
                           "ns_json triples: invalid JSON pointer (must start with '/'): %s", origP);
        return NULL;
    }

    Tcl_DStringInit(&listDs);
    Tcl_DStringInit(&segDs);

    start = 1;
    for (i = 1; i <= len; i++) {
        if (i == len || p[i] == '/') {
            const char *seg = p + start;
            TCL_SIZE_T segLen = i - start;

            Tcl_DStringSetLength(&segDs, 0);
            if (JsonKeyPathUnescapeSegment(&segDs, seg, segLen, NS_TRUE) != NS_OK) {
                /*
                 * RFC 6901 allows only ~0 and ~1 escapes.
                 */
                Ns_TclPrintfResult(interp,
                                   "ns_json triples: invalid JSON pointer escape in: %s", origP);
                goto fail;
            }

            Tcl_DStringAppendElement(&listDs, segDs.string);
            start = i + 1;
        }
    }

    pathObj = DStringToObj(&listDs);

 fail:
    Tcl_DStringFree(&segDs);
    Tcl_DStringFree(&listDs);
    return pathObj;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesGetPath --
 *
 *      Determine the effective path list for triples operations from either
 *      a Tcl path list or a JSON Pointer string.
 *
 *      Exactly one of pathObj and pointerObj must be provided.  When pathObj
 *      is given, it is used as-is.  When pointerObj is given, it is parsed as
 *      an RFC 6901 JSON Pointer and converted into a Tcl list of reference
 *      tokens suitable for triples navigation.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on invalid arguments or conversion errors.
 *
 * Side effects:
 *      On success, *outPathObj is set to the effective path list object.
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonTriplesGetPath(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *pointerObj,
                   Tcl_Obj **outPathObj)
{
    if (pointerObj != NULL) {
        TCL_SIZE_T  pointerLen;
        const char *pointerString = Tcl_GetStringFromObj(pointerObj, &pointerLen);
        Tcl_Obj    *pObj = JsonPointerToPathObj(interp, pointerString, pointerLen);

        if (pObj == NULL) {
            return NS_ERROR;
        }
        *outPathObj = pObj;
        return NS_OK;
    }

    if (pathObj == NULL) {
        Ns_TclPrintfResult(interp, "ns_json triples: missing -path or -pointer");
        return NS_ERROR;
    }

    *outPathObj = pathObj;
    return NS_OK;
}

/*----------------------------------------------------------------------
 *
 * TriplesSetValue --
 *
 *      Replace the value at the specified path; the replacement may be scalar
 *      or a nested triples subtree.
 *
 *      Supports both legacy container triples (key type value ...) and the
 *      canonical root-wrapped form:
 *
 *          "" TYPE VALUE
 *
 *      For wrapped inputs, the path is applied to VALUE.  The returned triples
 *      preserve the wrapper.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------*/
static Ns_ReturnCode
TriplesSetValue(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *triplesObj,
                Tcl_Obj *newValueObj, JsonValueType vt,
                Tcl_Obj **resultTriplesPtr)
{
    Tcl_Obj   **pv;
    TCL_SIZE_T  pc;

    Tcl_Obj      *out;
    Tcl_Obj      *cur;
    JsonValueType rootVt = JSON_VT_AUTO;

    if (Tcl_ListObjGetElements(interp, pathObj, &pc, &pv) != TCL_OK) {
        return NS_ERROR;
    }
    if (pc == 0) {
        Ns_TclPrintfResult(interp, "ns_json triples setvalue: empty path");
        return NS_ERROR;
    }

    /*
     * Duplicate whole document (copy-on-write).
     */
    out = Tcl_DuplicateObj(triplesObj);
    cur = out;

    /*
     * Detect and unwrap root wrapper in the duplicated object.
     *
     * Wrapper form: "" TYPE VALUE
     */
    {
        Tcl_Obj   **lv;
        TCL_SIZE_T  lc;

        if (Tcl_ListObjGetElements(interp, out, &lc, &lv) == TCL_OK && lc == 3) {
            TCL_SIZE_T  klen;

            (void)Tcl_GetStringFromObj(lv[0], &klen);
            if (klen == 0) {
                JsonValueType tvt = JsonTypeObjToVt(lv[1]);

                if (tvt != JSON_VT_AUTO) {
                    /*
                     * The triples are wrapped.
                     */
                    Tcl_Obj *nestedDup;

                    rootVt = tvt;
                    /*
                     * For non-empty paths, we can only descend into container roots.
                     */
                    if (rootVt != JSON_VT_OBJECT && rootVt != JSON_VT_ARRAY) {
                        Ns_TclPrintfResult(interp,
                                           "ns_json triples setvalue: cannot descend into %s",
                                           Tcl_GetString(lv[1]));
                        return NS_ERROR;
                    }

                    /*
                     * Duplicate wrapped VALUE and replace it in-place, then traverse VALUE.
                     */
                    nestedDup = Tcl_DuplicateObj(lv[2]);
                    (void)Tcl_ListObjReplace(interp, out, 2, 1, 1, &nestedDup);
                    cur = nestedDup;
                }
            }
        }
    }

    /*
     * Recursive, duplicating rewrite starting at cur.
     */
    for (TCL_SIZE_T pi = 0; pi < pc; pi++) {
        Tcl_Obj   **lv;
        TCL_SIZE_T  lc;
        TCL_SIZE_T  base;

        if (Tcl_ListObjGetElements(interp, cur, &lc, &lv) != TCL_OK) {
            return NS_ERROR;
        }
        if (lc == 0 || (lc % 3) != 0) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples setvalue: input must be a list of {key|index type value} elements");
            return NS_ERROR;
        }

        base = -1;
        for (TCL_SIZE_T i = 0; i < lc; i += 3) {
            if (TripleKeyMatches(lv[i], pv[pi])) {
                base = i;
                break;
            }
        }
        if (base < 0) {
            Ns_TclPrintfResult(interp, "ns_json triples setvalue: no such element: %s",
                               Tcl_GetString(pv[pi]));
            return NS_ERROR;
        }

        if (pi == pc - 1) {

            /*
             * If caller provides a root-wrapped triples document as the replacement,
             * unwrap it here to avoid embedding the wrapper as a subtree (which would
             * later serialize as an object with key \"\"). The wrapper TYPE also gives
             * a reliable type for AUTO (and must match explicit -type when provided).
             */
            {
                Tcl_Obj      *wTypeObj = NULL;
                Tcl_Obj      *wValueObj = NULL;
                JsonValueType wVt;

                wVt = TriplesDetectRootWrapper(interp, newValueObj, &wTypeObj, &wValueObj);
                if (wVt != JSON_VT_AUTO) {
                    if (vt == JSON_VT_AUTO) {
                        vt = wVt;
                    } else if (vt != wVt) {
                        Ns_TclPrintfResult(interp,
                                           "ns_json triples setvalue: replacement wrapper type %s does not match -type %s",
                                           Tcl_GetString(wTypeObj),
                                           Tcl_GetString(JsonAtomObjs[vt]));
                        return NS_ERROR;
                    }
                    newValueObj = wValueObj;
                }
            }

            if (vt == JSON_VT_AUTO) {
                Tcl_Obj       *normObj, *oldTypeObj = lv[base+1];
                JsonValueType  oldVt = JsonTypeObjToVt(oldTypeObj);

                if (JsonIsNullObj(newValueObj)) {
                    vt = JSON_VT_NULL;

                } else if (oldVt != JSON_VT_AUTO) {
                    if (JsonValidateValue(interp, oldVt, newValueObj, &normObj, "triples setvalue") == NS_OK) {
                        vt = oldVt;
                        newValueObj = normObj;
                    } else {
                        if (oldVt == JSON_VT_NUMBER) {
                            return TCL_ERROR;  /* keep the “invalid number” message */
                        }
                        Tcl_ResetResult(interp); /* ignore normalization error; fall back below */
                    }
                }
                if (vt == JSON_VT_AUTO) {
                    vt = JsonValueTypeDetect(interp, newValueObj);
                    (void)JsonValidateValue(interp, vt, newValueObj, &newValueObj, "triples setvalue");
                }
            } else {
                if (vt == JSON_VT_NULL) {
                    newValueObj = JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING];
                }
            }

            /*
             * Replace TYPE and VALUE slots.
             */
            (void)Tcl_ListObjReplace(interp, cur, base + 1, 1, 1, &JsonAtomObjs[vt]);
            (void)Tcl_ListObjReplace(interp, cur, base + 2, 1, 1, &newValueObj);

            *resultTriplesPtr = out;
            return NS_OK;
        }

        /*
         * Descend: duplicate nested list at VALUE slot and replace it in-place,
         * then continue with that nested list.
         */
        {
            Tcl_Obj *nested = lv[base + 2];
            Tcl_Obj *nestedDup = Tcl_DuplicateObj(nested);

            (void)Tcl_ListObjReplace(interp, cur, base + 2, 1, 1, &nestedDup);
            cur = nestedDup;
        }
    }

    return NS_ERROR;
}

/*======================================================================
 * Function Implementations: Core parse helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonParseLiteral --
 *
 *      Parse a JSON literal at the current cursor position. Supported
 *      literals are: "true", "false", and "null".
 *
 * Results:
 *      NS_OK on success with *valueObjPtr and *typePtr set.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the literal on success.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseLiteral(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)
{
    const unsigned char *p = jp->p;

    if (jp->end - p >= 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') {
        jp->p += 4;
        *valueObjPtr = JsonAtomObjs[JSON_ATOM_TRUE];
        *valueTypePtr = JSON_VT_BOOL;
        return NS_OK;
    }
    if (jp->end - p >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') {
        jp->p += 5;
        *valueObjPtr = JsonAtomObjs[JSON_ATOM_FALSE];
        *valueTypePtr = JSON_VT_BOOL;
        return NS_OK;
    }
    if (jp->end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') {
        jp->p += 4;
        *valueObjPtr = jp->opt->nullValueObj;
        *valueTypePtr = JSON_VT_NULL;
        return NS_OK;
    }

    Ns_DStringPrintf(jp->errDsPtr,
                     "ns_json: parse error at byte %lu: invalid literal",
                     (unsigned long)(jp->p - jp->start));
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseNumber --
 *
 *      Parse a JSON number at the current cursor position. The returned
 *      Tcl object and the type string depend on the configured number
 *      mode (e.g., smart/double/string).
 *
 * Results:
 *      NS_OK on success with *valueObjPtr and *typePtr set.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the number lexeme on success.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseNumber(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)
{
    const unsigned char *s, *p;
    Tcl_Obj             *lexObj = NULL;
    bool                 sawFracOrExp;
    Ns_ReturnCode        status;

    status = JsonScanNumber(jp, &s, &p, &sawFracOrExp);
    if (status != NS_OK) {
        goto fail;
    }

    lexObj = Tcl_NewStringObj((const char *)s, (TCL_SIZE_T)(p - s));
    *valueObjPtr = lexObj;
    *valueTypePtr = JSON_VT_NUMBER;

    //fprintf(stderr, "JsonParseNumber validateNumbers %d sawFracOrExp %d\n", jp->opt->validateNumbers, sawFracOrExp);

    if (jp->opt->validateNumbers && sawFracOrExp) {
        double d;

        /*
         * If there is no fraction or exponent, all JSON numbers are valid Tcl
         * numbers
         */
        if (Tcl_GetDoubleFromObj(NULL, lexObj, &d) != TCL_OK) {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: invalid double",
                             (unsigned long)(s - jp->start));
            goto fail;
        }
        //fprintf(stderr, "... Tcl_GetDoubleFromObj returned %f\n", d);

        if (!NS_ISFINITE(d)) {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: number is not a finite Tcl double (%s)",
                             (unsigned long)(s - jp->start),
                             NS_ISINF(d) ? (d > 0 ? "Inf" : "-Inf") : "NaN");
            goto fail;
        }

    }
    return NS_OK;

 fail:
    if (lexObj != NULL) {
        Tcl_DecrRefCount(lexObj);
    }
    *valueObjPtr = NULL;
    return NS_ERROR;
}

/*
 * JsonParseStringToString --
 *
 *      Parse a JSON string starting at the opening double quote and return
 *      a pointer/length pair for the decoded UTF-8 bytes (without quotes).
 *
 *      The returned pointer is owned by the parser:
 *        - For strings without escapes, it points into the input buffer.
 *        - For strings with escapes, it points into jp->tmpDs.
 *
 *      The pointer is valid until the next call that mutates jp->tmpDs
 *      (i.e., typically the next JsonParseStringToString/JsonParseString).
 *
 * Results:
 *      NS_OK on success, NS_ERROR on parse error (errDsPtr filled).
 *
 * Side Effects:
 *      Advances jp->p to the first byte after the closing double quote.
 *      Resets and appends to jp->tmpDs on the escape (slow) path.
 */
static Ns_ReturnCode
JsonParseStringToString(JsonParser *jp, const char **outPtr, TCL_SIZE_T *outLenPtr)
{
    const unsigned char *p, *end, *dq, *bs;

    NS_NONNULL_ASSERT(jp != NULL);
    NS_NONNULL_ASSERT(outPtr != NULL);
    NS_NONNULL_ASSERT(outLenPtr != NULL);

    if (JsonExpect(jp, '"', "\"") != NS_OK) {
        return NS_ERROR;
    }

    p   = jp->p;      /* first char inside string */
    end = jp->end;

    /*
     * Fast path: find closing quote; if no backslash before it, return span.
     */
    dq = (const unsigned char *)memchr(p, '"', (size_t)(end - p));
    if (dq == NULL) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: unterminated string",
                         (unsigned long)(end - jp->start));
        return NS_ERROR;
    }

    bs = (const unsigned char *)memchr(p, '\\', (size_t)(dq - p));
    if (bs == NULL) {
        if (JsonCheckStringSpan(jp, p, dq, 0u) != NS_OK) {
            return NS_ERROR;
        }
        *outPtr    = (const char *)p;
        *outLenPtr = (TCL_SIZE_T)(dq - p);
        jp->p = dq + 1;  /* consume closing quote */
        return NS_OK;
    }

    /*
     * Slow path: decode escapes into jp->tmpDs and return its contents.
     */
    Tcl_DStringSetLength(&jp->tmpDs, 0);

    for (;;) {
        const unsigned char *q;

        bs = (const unsigned char *)memchr(p, '\\', (size_t)(end - p));
        dq = (const unsigned char *)memchr(p, '"',  (size_t)(end - p));

        if (bs == NULL) {
            q = dq;
        } else if (dq == NULL) {
            q = bs;
        } else {
            q = (bs < dq) ? bs : dq;
        }

        if (q == NULL) {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: unterminated string",
                             (unsigned long)(p - jp->start));
            return NS_ERROR;
        }

        if (q > p) {
            if (JsonCheckStringSpan(jp, p, q, (size_t)jp->tmpDs.length) != NS_OK) {
                return NS_ERROR;
            }
            Tcl_DStringAppend(&jp->tmpDs, (const char *)p, (TCL_SIZE_T)(q - p));
        }

        if (*q == '"') {
            jp->p = q + 1;
            *outPtr    = jp->tmpDs.string;
            *outLenPtr = jp->tmpDs.length;
            return NS_OK;
        }

        /* Escape at q == '\' */
        jp->p = q + 1;
        if (jp->p >= end) {
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: unexpected end in string escape",
                             (unsigned long)(jp->p - jp->start));
            return NS_ERROR;
        }

        {
            const char *bytes = NULL;
            size_t      len   = 0u;
            char        utf8[4];
            uint32_t    cp;

            int e = (int)(unsigned char)*jp->p++;

            switch (e) {
            case '"':  bytes = "\""; len = 1u; break;
            case '\\': bytes = "\\"; len = 1u; break;
            case '/':  bytes = "/";  len = 1u; break;
            case 'b':  bytes = "\b"; len = 1u; break;
            case 'f':  bytes = "\f"; len = 1u; break;
            case 'n':  bytes = "\n"; len = 1u; break;
            case 'r':  bytes = "\r"; len = 1u; break;
            case 't':  bytes = "\t"; len = 1u; break;

            case 'u':
                if (JsonDecodeUnicodeEscape(jp, &cp) != NS_OK) {
                    return NS_ERROR;
                }
                len = Ns_Utf8FromCodePoint(cp, utf8);
                if (len == 0u) {
                    Ns_DStringPrintf(jp->errDsPtr,
                                     "ns_json: parse error at byte %lu: invalid unicode scalar value",
                                     (unsigned long)(jp->p - jp->start));
                    return NS_ERROR;
                }
                bytes = utf8;
                break;

            default:
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: parse error at byte %lu: invalid escape '\\%c'",
                                 (unsigned long)((jp->p - 1) - jp->start), (char)e);
                return NS_ERROR;
            }

            if (JsonAppendDecoded(jp, q, bytes, len) != NS_OK) {
                return NS_ERROR;
            }
        }

        p = jp->p;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseString --
 *
 *      Parse a JSON string value starting at the current input position
 *      of the parser context.
 *
 *      The function consumes the opening quote, decodes the string
 *      contents (including escape sequences and Unicode escapes),
 *      validates that no unescaped control characters (< 0x20) occur,
 *      and advances the parser position to the byte following the
 *      closing quote.
 *
 *      On success, the parsed value is returned either as a Tcl_Obj
 *      (when required by the caller) and the value type is set to
 *      JSON_VT_STRING.
 *
 * Results:
 *      NS_OK on successful parsing of a JSON string.
 *      NS_ERROR on parse failure (e.g., invalid escape sequence,
 *      unterminated string, invalid Unicode escape).
 *
 * Side effects:
 *      On success, may allocate and return a Tcl_Obj representing the
 *      decoded string and advances the parser cursor.
 *      On error, records an appropriate parse error in the parser
 *      context for later reporting.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseString(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)
{
    const char *s;
    TCL_SIZE_T  len;

    NS_NONNULL_ASSERT(jp != NULL);
    NS_NONNULL_ASSERT(valueObjPtr != NULL);
    NS_NONNULL_ASSERT(valueTypePtr != NULL);

    if (JsonParseStringToString(jp, &s, &len) != NS_OK) {
        return NS_ERROR;
    }

    *valueObjPtr = Tcl_NewStringObj(s, len);
    *valueTypePtr = JSON_VT_STRING;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseKeyString --
 *
 *      Parse a JSON object member name (key) at the current parser
 *      position.
 *
 *      The function expects a JSON string (double-quoted) and decodes it
 *      like JsonParseString(), including escape handling and Unicode
 *      escapes, while enforcing the JSON constraint that unescaped control
 *      characters (< 0x20) are not permitted.
 *
 *      On success, the function returns the key either as an interned
 *      canonical representation (when key sharing is enabled) or as a
 *      freshly created Tcl_Obj, and advances the parser cursor to the byte
 *      following the closing quote.
 *
 * Results:
 *      NS_OK on successful parsing of the key string.
 *      NS_ERROR on parse failure (e.g., unterminated string, invalid escape,
 *      invalid Unicode sequence).
 *
 * Side effects:
 *      On success, may allocate a Tcl_Obj and may create/lookup an entry
 *      in the key sharing table; advances the parser cursor.
 *      On error, records an appropriate parse error in the parser context
 *      for later reporting.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseKeyString(JsonParser *jp, Tcl_Obj **keyObjPtr)
{
    const char *s = NULL;
    TCL_SIZE_T  len;

    if (JsonParseStringToString(jp, &s, &len) != NS_OK) {
        return NS_ERROR;
    }

#if NS_JSON_KEY_SHARING
    *keyObjPtr = JsonInternKeyObj(jp, s, len);
#else
    /*
     * Duplicate Tcl_Obj for every key (no sharing).
     */
    *keyObjPtr = Tcl_NewStringObj(s, len);
#endif
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseValue --
 *
 *      Parse a JSON value at the current cursor position. Dispatches to the
 *      appropriate parsing function for objects, arrays, strings, numbers,
 *      and literals. The value is returned either as Tcl structures (dict/list)
 *      or as a triples representation, depending on parser options.
 *
 * Results:
 *      NS_OK on success with *valueObjPtr and *typePtr set.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the parsed value on success.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseValue(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)
{
    int c;

    JsonSkipWs(jp);
    c = JsonPeek(jp);
    if (c < 0) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: unexpected end of input",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

    switch (c) {
    case '{':  return JsonParseObject(jp, valueObjPtr, valueTypePtr);
    case '[':  return JsonParseArray(jp, valueObjPtr, valueTypePtr);
    case '"':  return JsonParseString(jp, valueObjPtr, valueTypePtr);
    case 't':
    case 'f':
    case 'n':  return JsonParseLiteral(jp, valueObjPtr, valueTypePtr);
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            return JsonParseNumber(jp, valueObjPtr, valueTypePtr);
        }
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: unexpected character '%c'",
                         (unsigned long)(jp->p - jp->start), (char)c);
        return NS_ERROR;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseObject --
 *
 *      Parse a JSON object at the current cursor position. The parsed object
 *      is returned either as a Tcl dict (dict output) or as a triples list
 *      (triples output), depending on parser options. The type string is set
 *      to "object".
 *
 * Results:
 *      NS_OK on success with *valueObjPtr and *typePtr set.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the closing '}' on success.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseObject(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)
{
    Tcl_Obj    *accObj;       /* dict or list */
    Tcl_Obj    *keyObj = NULL;
    Tcl_Obj    *valObj = NULL;
    long        elemCount = 0;

    jp->depth++;
    if (jp->depth > jp->opt->maxDepth) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: max depth exceeded",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

    (void) JsonGet(jp); /* consume '{' */
    JsonSkipWs(jp);

    if (jp->opt->output == NS_JSON_OUTPUT_TRIPLES) {
        accObj = Tcl_NewListObj(0, NULL);
    } else {
        accObj = Tcl_NewDictObj();
    }

    if (JsonPeek(jp) == '}') {
        (void) JsonGet(jp);
        *valueObjPtr = accObj;
        *valueTypePtr = JSON_VT_OBJECT;
        jp->depth--;
        return NS_OK;
    }

    for (;;) {
        /*
         * key string
         */
        if (JsonParseKeyString(jp, &keyObj) != NS_OK) {
            jp->depth--;
            return NS_ERROR;
        }
        *valueTypePtr = JSON_VT_STRING;
        JsonSkipWs(jp);
        if (JsonExpect(jp, ':', "':'") != NS_OK) {
            jp->depth--;
            return NS_ERROR;
        }

        /*
         * value
         */
        if (JsonParseValue(jp, &valObj, valueTypePtr) != NS_OK) {
            jp->depth--;
            return NS_ERROR;
        }

        /*
         * append
         */
        if (jp->opt->output == NS_JSON_OUTPUT_TRIPLES) {
            JsonTriplesAppendVt(accObj, keyObj, *valueTypePtr, valObj);
        } else {
            if (Tcl_DictObjPut(NULL, accObj, keyObj, valObj) != TCL_OK) {
                jp->depth--;
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: internal error building dict");
                return NS_ERROR;
            }
        }

        elemCount++;
        if (jp->opt->maxContainer > 0 && elemCount > jp->opt->maxContainer) {
            jp->depth--;
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: max container size exceeded",
                             (unsigned long)(jp->p - jp->start));
            return NS_ERROR;
        }

        JsonSkipWs(jp);

        if (JsonPeek(jp) == ',') {
            (void) JsonGet(jp);
            JsonSkipWs(jp);
            continue;
        }
        if (JsonPeek(jp) == '}') {
            (void) JsonGet(jp);
            break;
        }

        jp->depth--;
        Ns_DStringPrintf(jp->errDsPtr, "ns_json: parse error at byte %lu: expected ',' or '}'",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

    *valueObjPtr = accObj;
    *valueTypePtr = JSON_VT_OBJECT;
    jp->depth--;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseArray --
 *
 *      Parse a JSON array at the current cursor position. The parsed array
 *      is returned either as a Tcl list (dict output) or as a triples list
 *      (triples output), depending on parser options. The type string is set
 *      to "array".
 *
 * Results:
 *      NS_OK on success with *valueObjPtr and *typePtr set.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the closing ']' on success.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseArray(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)
{
    Tcl_Obj *accObj;  /* list or triples list */
    Tcl_Obj *valObj;
    Tcl_WideInt   idx = 0;
    long     elemCount = 0;

    jp->depth++;
    if (jp->depth > jp->opt->maxDepth) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: max depth exceeded",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

    (void) JsonGet(jp); /* consume '[' */
    JsonSkipWs(jp);

    accObj = Tcl_NewListObj(0, NULL);

    if (JsonPeek(jp) == ']') {
        (void) JsonGet(jp);
        *valueObjPtr = accObj;
        *valueTypePtr = JSON_VT_ARRAY;
        jp->depth--;
        return NS_OK;
    }

    for (;;) {
        if (JsonParseValue(jp, &valObj, valueTypePtr) != NS_OK) {
            jp->depth--;
            return NS_ERROR;
        }
        if (*valueTypePtr == JSON_VT_AUTO) {
            Ns_Log(Error, "JsonParseArray: JsonParseValue returned AUTO type at byte %lu",
                   (unsigned long)(jp->p - jp->start));
        }
        if (jp->opt->output == NS_JSON_OUTPUT_TRIPLES) {
            Tcl_Obj *nameObj = Tcl_NewWideIntObj(idx);

            JsonTriplesAppendVt(accObj, nameObj, *valueTypePtr, valObj);
        } else {
            (void) Tcl_ListObjAppendElement(NULL, accObj, valObj);
        }

        idx++;
        elemCount++;
        if (jp->opt->maxContainer > 0 && elemCount > jp->opt->maxContainer) {
            jp->depth--;
            Ns_DStringPrintf(jp->errDsPtr,
                             "ns_json: parse error at byte %lu: max container size exceeded",
                             (unsigned long)(jp->p - jp->start));
            return NS_ERROR;
        }

        JsonSkipWs(jp);

        if (JsonPeek(jp) == ',') {
            (void) JsonGet(jp);
            JsonSkipWs(jp);
            continue;
        }
        if (JsonPeek(jp) == ']') {
            (void) JsonGet(jp);
            break;
        }

        jp->depth--;
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: expected ',' or ']'",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

    *valueObjPtr = accObj;
    *valueTypePtr = JSON_VT_ARRAY;
    jp->depth--;
    return NS_OK;
}


/*======================================================================
 * Function Implementations: Set-output parse helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonParseValueSet --
 *
 *      Parse a JSON value at the current cursor position and store it into
 *      the provided Ns_Set using a flattened key path representation. The
 *      current path is maintained in pathDsPtr, and the corresponding sidecar
 *      key for the type field is maintained in typeKeyDsPtr. The function
 *      indicates whether the value resulted in an empty container via
 *      *emptyPtr.
 *
 * Results:
 *      NS_OK on success with *typePtr set (type of the parsed value) and
 *      *emptyPtr indicating whether the parsed container was empty.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the parsed value. Adds entries to the Ns_Set and
 *      may modify the supplied path/typeKey DStrings.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseValueSet(JsonParser *jp, Ns_Set *set,
                  Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                  JsonValueType *valueTypePtr, bool *emptyPtr)
{
    Ns_ReturnCode rc    = NS_ERROR;
    Tcl_Obj      *valObj = NULL;
    const char   *s;
    TCL_SIZE_T    len;
    int           c;
    bool          empty = NS_FALSE;

    JsonSkipWs(jp);
    c = JsonPeek(jp);

    if (c < 0) {
        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: unexpected end of input",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

    switch (c) {
    case '{':
        if (JsonParseObjectSet(jp, set, pathDsPtr, typeKeyDsPtr, &empty) != NS_OK) {
            goto done;
        }
        *valueTypePtr = JSON_VT_OBJECT;
        rc = NS_OK;
        goto done;

    case '[':
        if (JsonParseArraySet(jp, set, pathDsPtr, typeKeyDsPtr, &empty) != NS_OK) {
            goto done;
        }
        *valueTypePtr = JSON_VT_ARRAY;
        rc = NS_OK;
        goto done;

    case '"':
        if (JsonParseStringToString(jp, &s, &len) != NS_OK) {
            goto done;
        }
        *valueTypePtr = JSON_VT_STRING;
        JsonFlattenToSet(set, pathDsPtr, typeKeyDsPtr, valObj, s, len, *valueTypePtr);
        rc = NS_OK;
        goto done;

    case 't':
    case 'f':
    case 'n':
        if (JsonParseLiteral(jp, &valObj, valueTypePtr) != NS_OK) {
            goto done;
        }
        Tcl_IncrRefCount(valObj);
        JsonFlattenToSet(set, pathDsPtr, typeKeyDsPtr, valObj, NULL, 0, *valueTypePtr);
        rc = NS_OK;
        goto done;

    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (JsonParseNumber(jp, &valObj, valueTypePtr) != NS_OK) {
                goto done;
            }
            Tcl_IncrRefCount(valObj);
            JsonFlattenToSet(set, pathDsPtr, typeKeyDsPtr, valObj, NULL, 0, *valueTypePtr);
            rc = NS_OK;
            goto done;
        }

        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: unexpected character '%c'",
                         (unsigned long)(jp->p - jp->start), (char)c);
        goto done;
    }

 done:
    if (valObj != NULL) {
        /*
         * Only decref if we incref'd. In this pattern we incref
         * immediately after successful parse for scalar cases.
         * For container cases valObj stays NULL.
         */
        Tcl_DecrRefCount(valObj);
    }
    *emptyPtr = empty;
    return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseObjectSet --
 *
 *      Parse a JSON object at the current cursor position and store its
 *      members into the provided Ns_Set using flattened path keys. The
 *      current path is maintained in pathDsPtr and the corresponding type
 *      sidecar key in typeKeyDsPtr. The function indicates whether the
 *      parsed object was empty via *emptyPtr.
 *
 * Results:
 *      NS_OK on success and *emptyPtr indicates whether the object contained
 *      no members.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the closing '}' on success. Adds entries to the
 *      Ns_Set and may modify the supplied path/typeKey DStrings.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseObjectSet(JsonParser *jp, Ns_Set *set,
                   Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                   bool *emptyPtr)
{
    JsonValueType vt;
    bool          empty = NS_TRUE;

    (void) JsonGet(jp); /* '{' */
    JsonSkipWs(jp);

    if (JsonPeek(jp) == '}') {
        (void) JsonGet(jp);
        empty = NS_TRUE;
        goto done;
    }

    for (;;) {
        const char *k;
        TCL_SIZE_T  savedLen, kLen;
        bool        childEmpty = NS_FALSE;

        empty = NS_FALSE;

        if (JsonParseStringToString(jp, &k, &kLen) != NS_OK) {
            return NS_ERROR;
        }

        JsonSkipWs(jp);
        if (JsonExpect(jp, ':', "':'") != NS_OK) {
            return NS_ERROR;
        }

        savedLen = pathDsPtr->length;
        JsonKeyPathAppendSegment(pathDsPtr, k, kLen);

        if (JsonParseValueSet(jp, set, pathDsPtr, typeKeyDsPtr, &vt, &childEmpty) != NS_OK) {
            return NS_ERROR;
        }

        Tcl_DStringSetLength(pathDsPtr, savedLen);

        JsonSkipWs(jp);
        if (JsonPeek(jp) == ',') {
            (void) JsonGet(jp);
            JsonSkipWs(jp);
            continue;
        }
        if (JsonPeek(jp) == '}') {
            (void) JsonGet(jp);
            break;
        }

        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: expected ',' or '}'",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

done:
    *emptyPtr = empty;

    /*
     * Container marker for empty object at a non-empty path.
     */
    if (empty && pathDsPtr->length > 0) {
        JsonSetPutType(set, typeKeyDsPtr,
                       pathDsPtr->string, pathDsPtr->length,
                       JSON_VT_OBJECT);
    }

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseArraySet --
 *
 *      Parse a JSON array at the current cursor position and store its
 *      elements into the provided Ns_Set using flattened path keys. Array
 *      indices are appended to the current path. The current path is
 *      maintained in pathDsPtr and the corresponding type sidecar key in
 *      typeKeyDsPtr. The function indicates whether the parsed array was
 *      empty via *emptyPtr.
 *
 * Results:
 *      NS_OK on success and *emptyPtr indicates whether the array contained
 *      no elements.
 *      NS_ERROR on parse error with a message appended to jp->errDsPtr.
 *
 * Side Effects:
 *      Advances jp->p past the closing ']' on success. Adds entries to the
 *      Ns_Set and may modify the supplied path/typeKey DStrings.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonParseArraySet(JsonParser *jp, Ns_Set *set,
                  Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                  bool *emptyPtr)
{
    size_t        idx = 0u;
    bool          empty = NS_TRUE;

    *emptyPtr = NS_FALSE;

    (void) JsonGet(jp); /* consume '[' */
    JsonSkipWs(jp);

    if (JsonPeek(jp) == ']') {
        (void) JsonGet(jp);
        empty = NS_TRUE;
        goto done;
    }

    for (;;) {
        TCL_SIZE_T savedLen;
        JsonValueType vt;
        bool childEmpty = NS_FALSE;

        empty = NS_FALSE;

        savedLen = pathDsPtr->length;
        JsonKeyPathAppendIndex(pathDsPtr, idx);

        if (JsonParseValueSet(jp, set, pathDsPtr, typeKeyDsPtr, &vt, &childEmpty) != NS_OK) {
            return NS_ERROR;
        }

        Tcl_DStringSetLength(pathDsPtr, savedLen);

        idx++;

        JsonSkipWs(jp);

        if (JsonPeek(jp) == ',') {
            (void) JsonGet(jp);
            JsonSkipWs(jp);
            continue;
        }
        if (JsonPeek(jp) == ']') {
            (void) JsonGet(jp);
            break;
        }

        Ns_DStringPrintf(jp->errDsPtr,
                         "ns_json: parse error at byte %lu: expected ',' or ']'",
                         (unsigned long)(jp->p - jp->start));
        return NS_ERROR;
    }

done:
    *emptyPtr = empty;

    /*
     * Marker for empty arrays in nested position.
     * For top-level empty array, path is empty and we emit nothing here.
     */
    if (empty && pathDsPtr->length > 0) {
        JsonSetPutType(set, typeKeyDsPtr,
                       pathDsPtr->string, pathDsPtr->length,
                       JSON_VT_ARRAY);
    }

    return NS_OK;
}


/*======================================================================
 * Function Implementations: Set population helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSetPutValue --
 *
 *      Store a flattened JSON value into the provided Ns_Set under the given
 *      key.  This helper encapsulates the Ns_Set insertion policy used by the
 *      JSON flattener.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Adds or updates an entry in the Ns_Set.
 *
 *----------------------------------------------------------------------
 */
static void
JsonSetPutValue(Ns_Set *set, const char *key, TCL_SIZE_T keyLen,
                const char *val, TCL_SIZE_T valLen)
{
    (void) Ns_SetPutSz(set, key, keyLen, val, valLen);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSetPutType --
 *
 *      Store the JSON type information for a flattened value.  The type is
 *      recorded in a sidecar key derived from the base key (typically by
 *      appending ".type").  This helper generates/uses the sidecar key in
 *      typeKeyDsPtr and inserts the type string into the Ns_Set.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Adds or updates an entry in the Ns_Set for the type sidecar key and
 *      may modify typeKeyDsPtr.
 *
 *----------------------------------------------------------------------
 */
static void
JsonSetPutType(Ns_Set *set, Tcl_DString *typeKeyDsPtr,
               const char *key, TCL_SIZE_T keyLen,
               JsonValueType vt)
{
    const char *typeString;
    TCL_SIZE_T len;

    JsonKeyPathMakeTypeKey(typeKeyDsPtr, key, keyLen);
    typeString = Tcl_GetStringFromObj(JsonAtomObjs[vt], &len);
    JsonSetPutValue(set,
                    typeKeyDsPtr->string,
                    typeKeyDsPtr->length,
                    typeString, len);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonFlattenToSet --
 *
 *      Store a parsed JSON value into an Ns_Set using the flattened key-path
 *      representation.
 *
 *      The function uses the current key path stored in pathDsPtr and the
 *      corresponding type sidecar key in typeKeyDsPtr.  The JSON value type
 *      vt determines the emitted "<key>.type" entry.  The value is taken from
 *      valueObj when non-NULL, otherwise from (valStr,valLen).
 *
 *      For ns_set output, the top-level container marker has an empty key
 *      path; in that case no key/value entry is emitted.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Adds entries to the Ns_Set and may read/modify pathDsPtr/typeKeyDsPtr.
 *
 *----------------------------------------------------------------------
 */
static void
JsonFlattenToSet(Ns_Set *set,
                 const Tcl_DString *pathDsPtr,
                 Tcl_DString *typeKeyDsPtr,
                 Tcl_Obj *valueObj,
                 const char *valStr, TCL_SIZE_T valLen,
                 JsonValueType vt)
{
    const char *key;
    TCL_SIZE_T  keyLen;

    NS_NONNULL_ASSERT(valueObj == NULL || valStr == NULL);

    key = pathDsPtr->string;
    keyLen = pathDsPtr->length;

    /*
     * For ns_set output it is better to use top-level container,
     * so keyLen==0 should only happen for that top-level container marker
     * case. We simply avoid emitting empty keys.
     */
    if (keyLen != 0) {
        /*
         * Always emit "<key>.type".
         */
        JsonSetPutType(set, typeKeyDsPtr, key, keyLen, vt);

        /*
         * Store key and value
         */
        if (valueObj != NULL) {
            valStr = Tcl_GetStringFromObj(valueObj, &valLen);
        }
        JsonSetPutValue(set, key, keyLen, valStr, valLen);
    }
}


/*======================================================================
 * Function Implementations: Key/path helpers for set and pointer handling.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonKeySplitSidecarField --
 *
 *      Split a flattened JSON key into its base key and optional sidecar
 *      field name.  Recognized sidecar suffixes (e.g., ".type") are removed
 *      from the returned base length and the field name is returned via
 *      fieldPtr/fieldLenPtr.
 *
 * Results:
 *      Length of the base key (without sidecar suffix). When no sidecar is
 *      present, the returned length equals keyLen and *fieldLenPtr is set to 0.
 *
 * Side Effects:
 *      Stores pointers/lengths for the field name in fieldPtr/fieldLenPtr.
 *
 *----------------------------------------------------------------------
 */
static TCL_SIZE_T
JsonKeySplitSidecarField(const char *key, TCL_SIZE_T keyLen,
                      const char **fieldPtr, TCL_SIZE_T *fieldLenPtr)
{
    if (keyLen >= (TCL_SIZE_T)NS_JSON_TYPE_SUFFIX_LEN
        && memcmp(key + keyLen - (TCL_SIZE_T)NS_JSON_TYPE_SUFFIX_LEN,
                  NS_JSON_TYPE_SUFFIX, NS_JSON_TYPE_SUFFIX_LEN) == 0) {
        *fieldPtr = "type";
        *fieldLenPtr = 4;
        return keyLen - (TCL_SIZE_T)NS_JSON_TYPE_SUFFIX_LEN;
    }
    *fieldPtr = NULL;
    *fieldLenPtr = 0;
    return keyLen;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyPathEscapeSegment --
 *
 *      Append a single path segment in escaped form to the provided
 *      Tcl_DString.  The escaping scheme is used for flattened keys and is
 *      compatible with JSON-Pointer style escaping:
 *
 *          "~" -> "~0"
 *          "/" -> "~1"
 *          "." -> "~2"
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Appends bytes to dsPtr.
 *
 *----------------------------------------------------------------------
 */
static void
JsonKeyPathEscapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen, bool rfc6901)
{
    const char *p = seg;
    const char *end = seg + segLen;

    while (p < end) {
        char c = *p++;

        if (c == '~') {
            Tcl_DStringAppend(dsPtr, "~0", 2);
        } else if (c == '/') {
            Tcl_DStringAppend(dsPtr, "~1", 2);
        } else if (c == '.' && !rfc6901) {
            Tcl_DStringAppend(dsPtr, "~2", 2);
        } else {
            Tcl_DStringAppend(dsPtr, &c, 1);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyPathUnescapeSegment --
 *
 *      Unescape a previously escaped path segment and append the unescaped
 *      bytes to the provided Tcl_DString.  This is the inverse of
 *      JsonKeyPathEscapeSegment().
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Appends bytes to dsPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonKeyPathUnescapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen, bool rfc6901)
{
    const char *p = seg;
    const char *end = seg + segLen;

    while (p < end) {
        char c = *p++, e;

        if (c == '~') {
            if (p >= end) {
                if (rfc6901) {
                    return NS_ERROR;  /* dangling '~' */
                }
                Tcl_DStringAppend(dsPtr, "~", 1);
                break;
            }

            e = *p++;

            switch (e) {
            case '0':
                Tcl_DStringAppend(dsPtr, "~", 1);
                break;

            case '1':
                Tcl_DStringAppend(dsPtr, "/", 1);
                break;

            case '2':
                if (rfc6901) {
                    return NS_ERROR;
                }
                Tcl_DStringAppend(dsPtr, ".", 1);
                break;

            default:
                if (rfc6901) {
                    return NS_ERROR;
                }
                Tcl_DStringAppend(dsPtr, "~", 1);
                Tcl_DStringAppend(dsPtr, &e, 1);
                break;
            }

        } else {
            Tcl_DStringAppend(dsPtr, &c, 1);
        }
    }

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyPathAppendSegment --
 *
 *      Append a path separator and an escaped segment to the current key
 *      path held in dsPtr.  This helper is used while flattening JSON
 *      containers into an Ns_Set, where nested structure is represented
 *      by "/"-separated path segments.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Appends bytes to dsPtr and updates the current path in-place.
 *
 *----------------------------------------------------------------------
 */
static void
JsonKeyPathAppendSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen)
{
    if (dsPtr->length > 0) {
        Tcl_DStringAppend(dsPtr, "/", 1);
    }
    JsonKeyPathEscapeSegment(dsPtr, seg, segLen, NS_FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyPathAppendIndex --
 *
 *      Append a path separator and a decimal array index to the current key
 *      path held in dsPtr.  This helper is used while flattening JSON arrays
 *      into an Ns_Set.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Appends bytes to dsPtr and updates the current path in-place.
 *
 *----------------------------------------------------------------------
 */
static void
JsonKeyPathAppendIndex(Tcl_DString *dsPtr, size_t idx)
{
    char buf[TCL_INTEGER_SPACE];
    int  n = ns_uint64toa(buf, idx);

    if (dsPtr->length > 0) {
        Tcl_DStringAppend(dsPtr, "/", 1);
    }
    Tcl_DStringAppend(dsPtr, buf, n);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyPathMakeTypeKey --
 *
 *      Construct the type sidecar key for the given base key.  The resulting
 *      key is stored in typeKeyDsPtr and is used to record the JSON type
 *      (e.g., "string", "int", "array") for a flattened value entry.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Overwrites the contents of typeKeyDsPtr with the generated sidecar key.
 *
 *----------------------------------------------------------------------
 */
static void
JsonKeyPathMakeTypeKey(Tcl_DString *typeKeyDsPtr, const char *key, TCL_SIZE_T keyLen)
{
    Tcl_DStringSetLength(typeKeyDsPtr, 0);
    Tcl_DStringAppend(typeKeyDsPtr, key, keyLen);
    Tcl_DStringAppend(typeKeyDsPtr, NS_JSON_TYPE_SUFFIX, NS_JSON_TYPE_SUFFIX_LEN);
}

/*======================================================================
 * Function Implementations: JSON emission helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesValueToJson --
 *
 *      Convert a JSON value represented in triples form into its JSON
 *      text representation.
 *
 *      The value is emitted into a Tcl_DString and returned as a Tcl_Obj.
 *      Depending on the flags, the output may be pretty-printed and/or
 *      numeric lexemes may be validated during emission.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Allocates a Tcl object containing the generated JSON text and
 *      stores it in *outObjPtr.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTriplesValueToJson(Tcl_Interp *interp, JsonValueType vt, Tcl_Obj *valueObj,
                       bool pretty, bool validateNumbers, Tcl_Obj **outObjPtr)
{
    Tcl_DString ds;
    int         depth = 0;
    int         isObject;

    Tcl_DStringInit(&ds);

    switch (vt) {
    case JSON_VT_OBJECT:
    case JSON_VT_ARRAY:
        /*
         * Ensure container triples are structurally valid and, if enabled,
         * validate number lexemes recursively.
         */
        if (JsonTriplesRequireValidContainerObj(interp, valueObj, NS_TRUE,
                                                validateNumbers,
                                                "triples getvalue") != NS_OK) {
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }

        isObject = (vt == JSON_VT_OBJECT);
        if (JsonEmitContainerFromTriples(interp, valueObj, isObject,
                                         validateNumbers,
                                         depth, pretty,
                                         &ds) != TCL_OK) {
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }
        break;

    case JSON_VT_STRING:
    case JSON_VT_NUMBER:
    case JSON_VT_BOOL:
    case JSON_VT_NULL: {
        /*
         * For scalars, reuse JsonEmitValueFromTriple() by providing
         * a typeObj that names the scalar type.
         *
         * If we already have cached type atoms/objs (JsonAtomObjs[]),
         * use them here rather than constructing new objects.
         */
        Tcl_Obj *typeObj = JsonAtomObjs[vt];

        if (JsonEmitValueFromTriple(interp, typeObj, valueObj,
                                    validateNumbers,
                                    depth, pretty,
                                    &ds) != TCL_OK) {
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }
        break;

    }

    case JSON_VT_AUTO:
    default:
        Ns_TclPrintfResult(interp,
                           "ns_json triples getvalue: invalid value type");
        Tcl_DStringFree(&ds);
        return TCL_ERROR;
    }

    *outObjPtr = DStringToObj(&ds);
    Tcl_DStringFree(&ds);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonEmitValueFromTriple --
 *
 *      Emit the JSON text representation for a typed triples value into the
 *      destination DString.  The triple type determines how valObj is
 *      serialized (e.g., quoted string, numeric lexeme, literals, or nested
 *      container serialization).
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on invalid triples input with an error
 *      message set in interp.
 *
 * Side Effects:
 *      Appends bytes to dsPtr.
 *
 *----------------------------------------------------------------------
 */
static int
JsonEmitValueFromTriple(Tcl_Interp *interp, Tcl_Obj *typeObj, Tcl_Obj *valObj,
                        bool validateNumbers, int depth, bool pretty, Tcl_DString *dsPtr)
{
    const char *t = Tcl_GetString(typeObj);

    if (JsonObjIsAtom(typeObj, JSON_ATOM_T_STRING)) {
        const char *s; TCL_SIZE_T len;
        s = Tcl_GetStringFromObj(valObj, &len);
        JsonAppendQuotedString(dsPtr, s, len);
        return TCL_OK;

    } else if (JsonObjIsAtom(typeObj, JSON_ATOM_T_NUMBER)) {
        /*
         * Emit numeric lexeme as-is (caller should ensure validity for "number").
         */
        const char *s; TCL_SIZE_T len;
        s = Tcl_GetStringFromObj(valObj, &len);
        if (validateNumbers) {
            Tcl_DString errDs;

            Tcl_DStringInit(&errDs);
            if (JsonValidateNumberString((const unsigned char *)s, (size_t)len, &errDs) != NS_OK) {
                Tcl_DStringResult(interp, &errDs);
                return TCL_ERROR;
            }
            Tcl_DStringFree(&errDs);
        }
        Tcl_DStringAppend(dsPtr, s, len);
        return TCL_OK;

    } else if (JsonObjIsAtom(typeObj, JSON_ATOM_T_BOOLEAN)
               || JsonObjIsAtom(typeObj, JSON_ATOM_BOOL)) {
        int b = 0;
        if (Tcl_GetBooleanFromObj(interp, valObj, &b) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_DStringAppend(dsPtr, b ? "true" : "false", b ? 4 : 5);
        return TCL_OK;

    } else if (JsonObjIsAtom(typeObj, JSON_ATOM_T_NULL)) {
        Tcl_DStringAppend(dsPtr, "null", 4);
        return TCL_OK;

    } else if (JsonObjIsAtom(typeObj, JSON_ATOM_T_OBJECT)) {
        return JsonEmitContainerFromTriples(interp, valObj, NS_TRUE, validateNumbers, depth, pretty, dsPtr);

    } else if (JsonObjIsAtom(typeObj, JSON_ATOM_T_ARRAY)) {
        return JsonEmitContainerFromTriples(interp, valObj, NS_FALSE, validateNumbers, depth, pretty, dsPtr);

    } else {
        Ns_TclPrintfResult(interp, "ns_json: unsupported triple type \"%s\"", t);
        return TCL_ERROR;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonEmitContainerFromTriples --
 *
 *      Emit the JSON text representation of a container described by a triples
 *      list.  When isObject is true, emit a JSON object and use the triple NAME
 *      elements as member names; otherwise emit a JSON array and ignore the
 *      triple NAME elements.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on invalid triples input with an error
 *      message set in interp.
 *
 * Side Effects:
 *      Appends bytes to dsPtr.
 *
 *----------------------------------------------------------------------
 */
static int
JsonEmitContainerFromTriples(Tcl_Interp *interp, Tcl_Obj *triplesObj, bool isObject,
                             bool validateNumbers, int depth, bool pretty,
                             Tcl_DString *dsPtr)
{
    Tcl_Obj  **ov;
    TCL_SIZE_T oc, i;
    bool       prettyMultiLine = NS_TRUE, first = NS_TRUE;

    if (Tcl_ListObjGetElements(interp, triplesObj, &oc, &ov) != TCL_OK) {
        return TCL_ERROR;
    }
    if (oc % 3 != 0) {
        Ns_TclPrintfResult(interp, "ns_json: triples length must be multiple of 3");
        return TCL_ERROR;
    }

    if (pretty) {
        if (isObject) {
            prettyMultiLine = !JsonObjectPrettySingleLine(interp, oc, ov);
        } else {
            prettyMultiLine = !JsonArrayPrettySingleLine(oc, ov);
        }
    }

    Tcl_DStringAppend(dsPtr, isObject ? "{" : "[", 1);
    if (pretty && oc > 0) {
        if (prettyMultiLine) {
            JsonPrettyIndent(dsPtr, depth + 1);
        } else if (isObject) {
            Tcl_DStringAppend(dsPtr, " ", 1);
        }
    }

    for (i = 0; i < oc; i += 3) {
        Tcl_Obj    *nameObj = ov[i];
        Tcl_Obj    *typeObj = ov[i+1];
        Tcl_Obj    *valObj  = ov[i+2];

        if (!first) {
            Tcl_DStringAppend(dsPtr, ",", 1);
            if (pretty) {
                if (prettyMultiLine) {
                    JsonPrettyIndent(dsPtr, depth + 1);
                } else {
                    Tcl_DStringAppend(dsPtr, " ", 1);
                }
            }
        }
        first = NS_FALSE;

        if (isObject) {
            TCL_SIZE_T  nlen;
            const char *name = Tcl_GetStringFromObj(nameObj, &nlen);

            JsonAppendQuotedString(dsPtr, name, nlen);
            Tcl_DStringAppend(dsPtr, pretty ? ": " : ":", pretty ? 2 : 1);
        }

        if (JsonEmitValueFromTriple(interp, typeObj, valObj, validateNumbers, depth + 1 , pretty, dsPtr) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    if (pretty && oc > 0) {
        if (prettyMultiLine) {
            JsonPrettyIndent(dsPtr, depth);
        } else if (isObject) {
            Tcl_DStringAppend(dsPtr, " ", 1);
        }
    }
    Tcl_DStringAppend(dsPtr, isObject ? "}" : "]", 1);
    return TCL_OK;
}

/*======================================================================
 * Function Implementations: Pretty-print helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonPrettyIndent --
 *
 *      Append a newline followed by indentation for pretty-printed JSON
 *      output.
 *
 *      Indentation uses two spaces per nesting level. The caller provides
 *      the current nesting depth, where depth 0 corresponds to the top
 *      level.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Appends to dsPtr.
 *
 *----------------------------------------------------------------------
 */
static void
JsonPrettyIndent(Tcl_DString *dsPtr, int depth)
{
    int i;
    Tcl_DStringAppend(dsPtr, "\n", 1);
    for (i = 0; i < depth * 2; i++) {
        Tcl_DStringAppend(dsPtr, " ", 1);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonObjectPrettySingleLine --
 *
 *      Determine whether a JSON object should be rendered on a single
 *      line in pretty-print mode.
 *
 *      As a first approximation, objects are rendered without line breaks
 *      when all values are either scalar JSON values or simple schema
 *      objects of the form
 *
 *          {type string VALUE}
 *          {type array  {0 string ...}}
 *
 *      Objects containing nested nontrivial objects or arrays are rendered
 *      across multiple lines.
 *
 *      The input is the VALUE part of an object in triples form, i.e. an
 *      object container-content triples list of the form
 *
 *          NAME TYPE VALUE NAME TYPE VALUE ...
 *
 * Results:
 *      NS_TRUE  when the object should be formatted on a single line,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonObjectPrettySingleLine(Tcl_Interp *interp, TCL_SIZE_T oc, Tcl_Obj **ov)
{
    JsonValueType vt;

    if (oc != 3) {
        return NS_FALSE;
    }

    vt = JsonTypeObjToVt(ov[1]);

    switch (vt) {
    case JSON_VT_STRING:
    case JSON_VT_NUMBER:
    case JSON_VT_BOOL:
    case JSON_VT_NULL:
        return NS_TRUE;

    case JSON_VT_ARRAY: {
        Tcl_Obj   **aov;
        TCL_SIZE_T  aoc;

        if (Tcl_ListObjGetElements(interp, ov[2], &aoc, &aov) != TCL_OK) {
            return NS_FALSE;
        }
        return JsonArrayPrettySingleLine(aoc, aov);
    }

    case JSON_VT_OBJECT: {
        Tcl_Obj   **iov;
        TCL_SIZE_T  ioc;

        if (Tcl_ListObjGetElements(interp, ov[2], &ioc, &iov) != TCL_OK) {
            return NS_FALSE;
        }
        return JsonObjectPrettySingleLine(interp, ioc, iov);
    }

    case JSON_VT_AUTO:
    default:
        return NS_FALSE;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonArrayPrettySingleLine --
 *
 *      Determine whether a JSON array should be rendered on a single
 *      line in pretty-print mode.
 *
 *      As a first approximation, arrays are rendered without line breaks
 *      when all elements are scalar values (string, number, boolean, or
 *      null).  Arrays containing nested objects or arrays are rendered
 *      across multiple lines.
 *
 *      The input is the VALUE part of an array in triples form, i.e. an
 *      array container-content triples list of the form
 *
 *          0 TYPE VALUE 1 TYPE VALUE ...
 *
 * Results:
 *      NS_TRUE  when the array should be formatted on a single line,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonArrayPrettySingleLine(TCL_SIZE_T  oc, Tcl_Obj   **ov)
{
    for (TCL_SIZE_T i = 0; i < oc; i += 3) {
        JsonValueType vt = JsonTypeObjToVt(ov[i + 1]);

        if (vt == JSON_VT_OBJECT || vt == JSON_VT_ARRAY || vt == JSON_VT_AUTO) {
            return NS_FALSE;
        }
    }

    return NS_TRUE;
}


/*======================================================================
 * Function Implementations: Sorting helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesSortObject --
 *
 *      Return a sorted copy of an object container represented in
 *      triples form.
 *
 *      The function sorts the NAME TYPE VALUE triples by key name to
 *      obtain a stable canonical ordering of object members.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Allocates a new Tcl list object containing the sorted triples
 *      and stores it in *sortedObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonTriplesSortObject(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj **sortedObjPtr)
{
    Tcl_Obj         **lv;
    TCL_SIZE_T        lc;
    Tcl_Obj          *sortedObj;
    JsonTripleEntry  *entries;
    TCL_SIZE_T        nentries;

    NS_NONNULL_ASSERT(sortedObjPtr != NULL);
    *sortedObjPtr = NULL;

    if (Tcl_ListObjGetElements(interp, triplesObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json: object triples length must be multiple of 3");
        return NS_ERROR;
    }

    nentries = lc / 3;
    entries = (JsonTripleEntry *)ns_malloc((size_t)nentries * sizeof(JsonTripleEntry));

    for (TCL_SIZE_T i = 0, j = 0; i < lc; i += 3, j++) {
        entries[j].nameObj  = lv[i + 0];
        entries[j].typeObj  = lv[i + 1];
        entries[j].valueObj = lv[i + 2];
    }

    qsort(entries, (size_t)nentries, sizeof(JsonTripleEntry), JsonTripleEntryNameCmp);

    sortedObj = Tcl_NewListObj(0, NULL);
    for (TCL_SIZE_T i = 0; i < nentries; i++) {
        JsonTriplesAppend(sortedObj,
                          entries[i].nameObj,
                          entries[i].typeObj,
                          entries[i].valueObj);
    }

    ns_free(entries);
    *sortedObjPtr = sortedObj;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTripleEntryNameCmp --
 *
 *      Comparison function used when sorting triples representing an
 *      object container.
 *
 *      The function compares the NAME element of two triples and
 *      returns the ordering required by qsort().
 *
 * Results:
 *      An integer less than, equal to, or greater than zero depending
 *      on the lexical ordering of the compared keys.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTripleEntryNameCmp(const void *a, const void *b)
{
    const JsonTripleEntry *ea = (const JsonTripleEntry *)a;
    const JsonTripleEntry *eb = (const JsonTripleEntry *)b;
    const char *sa = Tcl_GetString(ea->nameObj);
    const char *sb = Tcl_GetString(eb->nameObj);
    return strcmp(sa, sb);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaStringEntryCmp --
 *
 *      Comparison function used when sorting arrays of strings in
 *      schema objects.
 *
 *      This helper is primarily used when canonicalizing schema fields
 *      such as "required" or type unions, where stable lexicographic
 *      ordering is desired.
 *
 * Results:
 *      An integer less than, equal to, or greater than zero depending
 *      on the lexical ordering of the compared strings.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
JsonSchemaStringEntryCmp(const void *a, const void *b)
{
    const JsonStringEntry *ea = (const JsonStringEntry *)a;
    const JsonStringEntry *eb = (const JsonStringEntry *)b;

    return strcmp(Tcl_GetString(ea->valueObj), Tcl_GetString(eb->valueObj));
}

/*======================================================================
 * Function Implementations: Schema triples field access helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaFindField --
 *
 *      Locate a field in a schema object represented in triples form.
 *
 *      The function searches the NAME TYPE VALUE triples of triplesObj
 *      for an entry whose NAME matches nameObj.  When found, the index
 *      of the triple (the position of the NAME element) is returned via
 *      baseIdxPtr.
 *
 * Results:
 *      Returns NS_TRUE when the field is present, NS_FALSE otherwise.
 *
 * Side effects:
 *      On success, stores the index of the triple in *baseIdxPtr.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonSchemaFindField(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *nameObj,
                           TCL_SIZE_T *baseIdxPtr)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    *baseIdxPtr = TCL_INDEX_NONE;

    if (Tcl_ListObjGetElements(interp, triplesObj, &lc, &lv) != TCL_OK) {
        return NS_FALSE;
    }
    if ((lc % 3) != 0) {
        return NS_FALSE;
    }

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        if (TripleKeyMatches(lv[i], nameObj)) {
            *baseIdxPtr = i;
            return NS_TRUE;
        }
    }
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaGetField --
 *
 *      Retrieve a field from a schema object represented in triples form.
 *
 *      The function searches triplesObj for a triple with NAME equal to
 *      nameObj.  When found, the TYPE and VALUE elements of the triple
 *      can be returned to the caller via typeObjPtr and valueObjPtr.
 *
 * Results:
 *      Returns NS_TRUE when the field is present, NS_FALSE otherwise.
 *
 * Side effects:
 *      When the field exists, stores the TYPE and VALUE objects and
 *      optionally the base index of the triple in the provided output
 *      pointers.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonSchemaGetField(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *nameObj,
                          Tcl_Obj **typeObjPtr, Tcl_Obj **valueObjPtr, TCL_SIZE_T *baseIdxPtr)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc, base;

    if (typeObjPtr != NULL)  { *typeObjPtr = NULL; }
    if (valueObjPtr != NULL) { *valueObjPtr = NULL; }
    if (baseIdxPtr != NULL)  { *baseIdxPtr = TCL_INDEX_NONE; }

    if (!JsonSchemaFindField(interp, triplesObj, nameObj, &base)) {
        return NS_FALSE;
    }
    if (Tcl_ListObjGetElements(interp, triplesObj, &lc, &lv) != TCL_OK) {
        return NS_FALSE;
    }

    if (typeObjPtr != NULL)  { *typeObjPtr  = lv[base + 1]; }
    if (valueObjPtr != NULL) { *valueObjPtr = lv[base + 2]; }
    if (baseIdxPtr != NULL)  { *baseIdxPtr  = base; }

    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaSetField --
 *
 *      Set or replace a field in a schema object represented in triples
 *      form.
 *
 *      The function ensures that triplesObj contains a NAME TYPE VALUE
 *      triple for nameObj.  When the field already exists, its TYPE and
 *      VALUE elements are replaced.  Otherwise a new triple is appended.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Modifies triplesObj by replacing an existing triple or appending
 *      a new one.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaSetField(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *nameObj,
                          JsonValueType vt, Tcl_Obj *valueObj)
{
    TCL_SIZE_T base;

    if (JsonSchemaFindField(interp, triplesObj, nameObj, &base)) {
        Tcl_Obj *repl[3];

        repl[0] = nameObj;
        repl[1] = JsonAtomObjs[vt];
        repl[2] = valueObj;

        return Tcl_ListObjReplace(interp, triplesObj, base, 3, 3, repl) == TCL_OK ? NS_OK : NS_ERROR;
    }

    JsonTriplesAppendVt(triplesObj, nameObj, vt, valueObj);
    return TCL_OK;
}

/*======================================================================
 * Function Implementations: Schema derivation helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaNewTypeTriple --
 *
 *      Create a new schema fragment representing a simple JSON type.
 *
 *      The function allocates a new triples-form list containing a
 *      single NAME TYPE VALUE triple for the "type" field, where the
 *      VALUE element is the JSON type name specified by atom.
 *
 * Results:
 *      Returns a newly allocated Tcl list object representing the
 *      schema fragment.
 *
 * Side effects:
 *      Allocates a new Tcl object.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj*
JsonSchemaNewTypeTriple(JsonAtom atom)
{
    Tcl_Obj* schemaObj = Tcl_NewListObj(0, NULL);
    JsonTriplesAppendVt(schemaObj,
                      JsonAtomObjs[JSON_ATOM_TYPE], JSON_VT_STRING, JsonAtomObjs[atom]);
    return schemaObj;
}

/*
 * ----------------------------------------------------------------------*
 *
 * JsonSchemaFromValue --
 *
 *      Derive a schema fragment from one JSON value.
 *
 *      The returned schema fragment is encoded as object triples, so it can
 *      be serialized via JsonTriplesValueToJson() like other ns_json values.
 *
 *      Examples:
 *
 *          JSON string  ->  {type string string}
 *          JSON number  ->  {type string number}
 *          JSON null    ->  {type string null}
 *          JSON object  ->  {
 *                              type string object
 *                              properties object {...}
 *                           }
 *          JSON array   ->  {
 *                              type string array
 *                              items object {...}
 *                           }
 *
 * Results:
 *      NS_OK on success, NS_ERROR on malformed triples or allocation failure.
 *
 * Side effects:
 *      On success, *schemaObjPtr is set to an object-triples Tcl list.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaFromValue(Tcl_Interp *interp, JsonValueType vt, Tcl_Obj *valueObj,
                    bool includeRequired, Tcl_Obj **schemaObjPtr)
{
    NS_NONNULL_ASSERT(schemaObjPtr != NULL);

    switch (vt) {
    case JSON_VT_STRING:
        *schemaObjPtr = JsonSchemaNewTypeTriple(JSON_ATOM_T_STRING);
        return NS_OK;

    case JSON_VT_NUMBER:
        *schemaObjPtr = JsonSchemaNewTypeTriple(JSON_ATOM_T_NUMBER);
        return NS_OK;

    case JSON_VT_BOOL:
        *schemaObjPtr = JsonSchemaNewTypeTriple(JSON_ATOM_T_BOOLEAN);
        return NS_OK;

    case JSON_VT_NULL:
        *schemaObjPtr = JsonSchemaNewTypeTriple(JSON_ATOM_T_NULL);
        return NS_OK;

    case JSON_VT_OBJECT:
        return JsonSchemaFromObject(interp, valueObj, includeRequired, schemaObjPtr);

    case JSON_VT_ARRAY:
        return JsonSchemaFromArray(interp, valueObj, includeRequired, schemaObjPtr);

    case JSON_VT_AUTO:
    default:
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: unsupported value type");
        return NS_ERROR;
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * JsonSchemaFromObject --
 *
 *      Derive a schema fragment from an object container-content triples list.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      On success, *schemaObjPtr is set to a Tcl dict object.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaFromObject(Tcl_Interp *interp, Tcl_Obj *triplesObj,
                     bool includeRequired, Tcl_Obj **schemaObjPtr)
{
    Tcl_Obj   *schemaObj, *propertiesObj, *requiredObj;
    Tcl_Obj  **lv;
    TCL_SIZE_T lc;

    NS_NONNULL_ASSERT(schemaObjPtr != NULL);
    *schemaObjPtr = NULL;

    if (Tcl_ListObjGetElements(interp, triplesObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed object triples "
                           "(length not multiple of 3)");
        return NS_ERROR;
    }

    schemaObj     = Tcl_NewListObj(0, NULL);
    propertiesObj = Tcl_NewListObj(0, NULL);
    requiredObj   = Tcl_NewListObj(0, NULL);

    JsonTriplesAppendVt(schemaObj,
                        JsonAtomObjs[JSON_ATOM_TYPE], JSON_VT_STRING, JsonAtomObjs[JSON_ATOM_T_OBJECT]);

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        Tcl_Obj       *nameObj  = lv[i + 0];
        Tcl_Obj       *typeObj  = lv[i + 1];
        Tcl_Obj       *valueObj = lv[i + 2];
        Tcl_Obj       *childSchemaObj = NULL;
        JsonValueType  vt = JsonTypeObjToVt(typeObj);

        if (vt == JSON_VT_AUTO) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples schema: malformed object triples "
                               "(invalid type token)");
            return NS_ERROR;
        }
        if (JsonSchemaFromValue(interp, vt, valueObj, includeRequired,
                                &childSchemaObj) != NS_OK) {
            return NS_ERROR;
        }

        /*
         * properties.NAME = childSchema
         */
        JsonTriplesAppendVt(propertiesObj,
                            nameObj, JSON_VT_OBJECT, childSchemaObj);

        if (includeRequired) {
            Tcl_Obj *idxObj = Tcl_NewWideIntObj((Tcl_WideInt)(i / 3));

            JsonTriplesAppendVt(requiredObj,
                                idxObj, JSON_VT_STRING, nameObj);
        }
    }

    JsonTriplesAppendVt(schemaObj,
                        JsonAtomObjs[JSON_ATOM_PROPERTIES], JSON_VT_OBJECT, propertiesObj);

    if (includeRequired) {
        JsonTriplesAppendVt(schemaObj,
                            JsonAtomObjs[JSON_ATOM_REQUIRED], JSON_VT_ARRAY, requiredObj);
    }

    *schemaObjPtr = schemaObj;
    return NS_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * JsonSchemaFromArray --
 *
 *      Derive a schema fragment from an array container-content triples list.
 *
 *      Array item schemas are merged into a single "items" schema.  When
 *      incompatible structures are observed, the merge falls back to "anyOf".
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      On success, *schemaObjPtr is set to a Tcl dict object.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaFromArray(Tcl_Interp *interp, Tcl_Obj *triplesObj,
                    bool includeRequired, Tcl_Obj **schemaObjPtr)
{
    Tcl_Obj   *schemaObj, *itemsObj = NULL;
    Tcl_Obj  **lv;
    TCL_SIZE_T lc;

    NS_NONNULL_ASSERT(schemaObjPtr != NULL);
    *schemaObjPtr = NULL;

    if (Tcl_ListObjGetElements(interp, triplesObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed array triples "
                           "(length not multiple of 3)");
        return NS_ERROR;
    }

    schemaObj = Tcl_NewListObj(0, NULL);

    JsonTriplesAppendVt(schemaObj,
                        JsonAtomObjs[JSON_ATOM_TYPE], JSON_VT_STRING, JsonAtomObjs[JSON_ATOM_T_ARRAY]);

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        Tcl_Obj       *typeObj  = lv[i + 1];
        Tcl_Obj       *valueObj = lv[i + 2];
        Tcl_Obj       *childSchemaObj = NULL;
        Tcl_Obj       *mergedItemsObj = NULL;
        JsonValueType  vt = JsonTypeObjToVt(typeObj);

        if (vt == JSON_VT_AUTO) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples schema: malformed array triples "
                               "(invalid type token)");
            return NS_ERROR;
        }
        if (JsonSchemaFromValue(interp, vt, valueObj, includeRequired,
                                &childSchemaObj) != NS_OK) {
            return NS_ERROR;
        }

        if (itemsObj == NULL) {
            itemsObj = childSchemaObj;
        } else {
            if (JsonSchemaMerge(interp, itemsObj, childSchemaObj,
                                &mergedItemsObj) != NS_OK) {
                return NS_ERROR;
            }
            itemsObj = mergedItemsObj;
        }
    }

    /*
     * Empty array => unconstrained items schema: {}
     */
    if (itemsObj == NULL) {
        itemsObj = Tcl_NewListObj(0, NULL);
    }

    JsonTriplesAppendVt(schemaObj,
                        JsonAtomObjs[JSON_ATOM_ITEMS], JSON_VT_OBJECT, itemsObj);

    *schemaObjPtr = schemaObj;
    return NS_OK;
}

/*======================================================================
 * Function Implementations: Schema merge helpers.
 *======================================================================
 */

/*
 * ----------------------------------------------------------------------
 *
 * JsonSchemaMerge --
 *
 *      Merge two schema fragments derived from observed values.
 *
 *      This is intentionally conservative.  Matching scalar types are kept.
 *      Matching object/array schemas are merged recursively.  Incompatible
 *      shapes fall back to an "anyOf" schema containing both inputs.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      On success, *mergedObjPtr is set to a schema dict object.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMerge(Tcl_Interp *interp, Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj,
                Tcl_Obj **mergedObjPtr)
{
    Tcl_Obj      *type1Obj = NULL, *type2Obj = NULL;
    Ns_ReturnCode result = NS_OK;

    *mergedObjPtr = NULL;

    {
        Tcl_Obj *types1Obj = NULL, *types2Obj = NULL;
        Tcl_Obj *mergedTypesObj = NULL, *typeSchemaObj = NULL;

        if (JsonSchemaIsTypeOnlySchema(interp, schema1Obj, NULL, NULL)
            && JsonSchemaIsTypeOnlySchema(interp, schema2Obj, NULL, NULL)) {
            if (JsonSchemaExtractTypeUnion(interp, schema1Obj, &types1Obj) != NS_OK
                || JsonSchemaExtractTypeUnion(interp, schema2Obj, &types2Obj) != NS_OK
                || JsonSchemaMergeTypeUnion(interp, types1Obj, types2Obj, &mergedTypesObj) != NS_OK
                || JsonSchemaBuildTypeUnion(interp, mergedTypesObj, &typeSchemaObj) != NS_OK) {
                return NS_ERROR;
            }
            *mergedObjPtr = typeSchemaObj;
            return NS_OK;
        }
    }

    if (!JsonSchemaGetField(interp, schema1Obj, JsonAtomObjs[JSON_ATOM_TYPE],
                                   NULL, &type1Obj, NULL)
        || !JsonSchemaGetField(interp, schema2Obj, JsonAtomObjs[JSON_ATOM_TYPE],
                                      NULL, &type2Obj, NULL)) {
        *mergedObjPtr = JsonSchemaBuildAnyOf2(schema1Obj, schema2Obj);

    } else if (strcmp(Tcl_GetString(type1Obj), Tcl_GetString(type2Obj)) == 0) {

        if (JsonObjIsAtom(type1Obj, JSON_ATOM_T_OBJECT)) {
            return JsonSchemaMergeObject(interp, schema1Obj, schema2Obj, mergedObjPtr);
        }

        if (JsonObjIsAtom(type1Obj, JSON_ATOM_T_ARRAY)) {
            return JsonSchemaMergeArray(interp, schema1Obj, schema2Obj, mergedObjPtr);
        }

        *mergedObjPtr = Tcl_DuplicateObj(schema1Obj);

    } else {
        *mergedObjPtr = JsonSchemaBuildAnyOf2(schema1Obj, schema2Obj);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMergeObject --
 *
 *      Merge two schema fragments of type object.
 *
 *      Object properties are merged by name. Matching properties are merged
 *      recursively, while properties present in only one schema are copied
 *      unchanged to the result.
 *
 *      When a "required" field is present, the merged schema uses
 *      intersection semantics, i.e. a property remains required only when it
 *      is required in both input schemas.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on malformed schema triples or Tcl errors.
 *
 * Side effects:
 *      On success, *mergedObjPtr is set to a newly allocated schema object
 *      in triples form.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMergeObject(Tcl_Interp *interp, Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj, Tcl_Obj **mergedObjPtr) {
    Tcl_Obj *mergedObj = Tcl_DuplicateObj(schema1Obj);
    Tcl_Obj *props1Obj = NULL, *props2Obj = NULL;
    Tcl_Obj *required1Obj = NULL, *required2Obj = NULL, *requiredOutObj = NULL;
    TCL_SIZE_T base;
    Tcl_Obj   **lv;
    TCL_SIZE_T lc;

    if (!JsonSchemaGetField(interp, mergedObj, JsonAtomObjs[JSON_ATOM_PROPERTIES],
                                   NULL, &props1Obj, &base)) {
        props1Obj = Tcl_NewListObj(0, NULL);
        if (JsonSchemaSetField(interp, mergedObj,
                                      JsonAtomObjs[JSON_ATOM_PROPERTIES],
                                      JSON_VT_OBJECT, props1Obj) != NS_OK) {
            return NS_ERROR;
        }
    } else if (Tcl_IsShared(props1Obj)) {
        Tcl_Obj *dup = Tcl_DuplicateObj(props1Obj);

        if (Tcl_ListObjGetElements(interp, mergedObj, &lc, &lv) != TCL_OK) {
            return NS_ERROR;
        }
        if (Tcl_ListObjReplace(interp, mergedObj, base + 2, 1, 1, &dup) != TCL_OK) {
            return NS_ERROR;
        }
        props1Obj = dup;
    }

    if (!JsonSchemaGetField(interp, schema2Obj, JsonAtomObjs[JSON_ATOM_PROPERTIES],
                                   NULL, &props2Obj, NULL)) {
        props2Obj = Tcl_NewListObj(0, NULL);
    }

    if (Tcl_ListObjGetElements(interp, props2Obj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed properties triples");
        return NS_ERROR;
    }

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        Tcl_Obj   *nameObj       = lv[i + 0];
        Tcl_Obj   *val2Obj       = lv[i + 2];
        Tcl_Obj   *val1Obj       = NULL;
        Tcl_Obj   *mergedPropObj = NULL;
        TCL_SIZE_T propBase;

        if (!JsonSchemaGetField(interp, props1Obj, nameObj, NULL, &val1Obj, &propBase)) {
            JsonTriplesAppendVt(props1Obj,
                                nameObj, JSON_VT_OBJECT, val2Obj);
        } else {
            Tcl_Obj *repl[3];

            if (JsonSchemaMerge(interp, val1Obj, val2Obj, &mergedPropObj) != NS_OK) {
                return NS_ERROR;
            }

            repl[0] = nameObj;
            repl[1] = JsonAtomObjs[JSON_VT_OBJECT];
            repl[2] = mergedPropObj;

            if (Tcl_ListObjReplace(interp, props1Obj, propBase, 3, 3, repl) != TCL_OK) {
                return NS_ERROR;
            }
        }
    }

    /*
     * required = intersection(required1, required2)
     *
     * If one side does not have a required field, treat it as empty.
     */
    (void)JsonSchemaGetField(interp, schema1Obj, JsonAtomObjs[JSON_ATOM_REQUIRED],
                                    NULL, &required1Obj, NULL);
    (void)JsonSchemaGetField(interp, schema2Obj, JsonAtomObjs[JSON_ATOM_REQUIRED],
                                    NULL, &required2Obj, NULL);

    if (required1Obj != NULL || required2Obj != NULL) {
        if (JsonSchemaRequiredIntersection(interp, required1Obj, required2Obj,
                                           &requiredOutObj) != NS_OK) {
            return NS_ERROR;
        }

        if (JsonSchemaSetField(interp, mergedObj,
                                      JsonAtomObjs[JSON_ATOM_REQUIRED],
                                      JSON_VT_ARRAY,
                                      requiredOutObj) != NS_OK) {
            return NS_ERROR;
        }
    }

    *mergedObjPtr = mergedObj;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMergeArray --
 *
 *      Merge two array schemas into a single schema description.
 *
 *      The function combines the "items" specifications of the two
 *      input schemas and produces a schema describing arrays that are
 *      compatible with both inputs.  Item schemas are merged according
 *      to the general schema merge rules.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a new schema object and stores it in *mergedObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMergeArray(Tcl_Interp *interp, Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj, Tcl_Obj **mergedObjPtr)
{
    Tcl_Obj *mergedObj = Tcl_DuplicateObj(schema1Obj);
    Tcl_Obj *items1Obj = NULL, *items2Obj = NULL, *mergedItemsObj = NULL;

    if (!JsonSchemaGetField(interp, schema1Obj, JsonAtomObjs[JSON_ATOM_ITEMS],
                                   NULL, &items1Obj, NULL)) {
        items1Obj = Tcl_NewListObj(0, NULL);
    }
    if (!JsonSchemaGetField(interp, schema2Obj, JsonAtomObjs[JSON_ATOM_ITEMS],
                                   NULL, &items2Obj, NULL)) {
        items2Obj = Tcl_NewListObj(0, NULL);
    }

    if (JsonSchemaMerge(interp, items1Obj, items2Obj, &mergedItemsObj) != NS_OK) {
        return NS_ERROR;
    }
    if (JsonSchemaSetField(interp, mergedObj,
                                  JsonAtomObjs[JSON_ATOM_ITEMS],
                                  JSON_VT_OBJECT,
                                  mergedItemsObj) != NS_OK) {
        return NS_ERROR;
    }

    *mergedObjPtr = mergedObj;
    return NS_OK;
}


/*======================================================================
 * Function Implementations: Schema canonicalization helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaCanonicalize --
 *
 *      Produce a canonical representation of a schema object.
 *
 *      The function normalizes the structure of schemaObj by enforcing
 *      a stable ordering of schema fields and recursively canonicalizing
 *      nested schema objects.  Fields such as "type", "properties",
 *      "items", and "required" are emitted in a fixed order and arrays
 *      such as "required" are sorted to obtain deterministic output.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a new Tcl object containing the canonicalized schema
 *      and stores it in *canonObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaCanonicalize(Tcl_Interp *interp, Tcl_Obj *schemaObj, Tcl_Obj **canonObjPtr)
{
    Tcl_Obj *outObj;
    Tcl_Obj *typeTypeObj = NULL, *typeValueObj = NULL;
    Tcl_Obj *canonTypeObj = NULL;
    Tcl_Obj *propertiesObj = NULL, *canonPropsObj = NULL;
    Tcl_Obj *itemsObj = NULL, *canonItemsObj = NULL;
    Tcl_Obj *requiredObj = NULL, *canonRequiredObj = NULL;
    Tcl_Obj *anyOfObj = NULL, *schemaDeclObj = NULL;
    Tcl_Obj *canonAnyOfObj = NULL;

    *canonObjPtr = NULL;
    outObj = Tcl_NewListObj(0, NULL);

    /*
     * $schema first
     */
    if (JsonSchemaGetField(interp, schemaObj, JsonAtomObjs[JSON_ATOM_SCHEMA],
                                  NULL, &schemaDeclObj, NULL)) {
        JsonTriplesAppendVt(outObj,
                            JsonAtomObjs[JSON_ATOM_SCHEMA], JSON_VT_STRING, schemaDeclObj);
    }

    /*
     * type
     */
    if (JsonSchemaGetField(interp, schemaObj, JsonAtomObjs[JSON_ATOM_TYPE],
                                  &typeTypeObj, &typeValueObj, NULL)) {
        if (JsonSchemaCanonicalizeTypeField(interp, typeTypeObj, typeValueObj,
                                            &canonTypeObj) != NS_OK) {
            return NS_ERROR;
        }
        JsonTriplesAppendVt(outObj,
                            JsonAtomObjs[JSON_ATOM_TYPE], JsonTypeObjToVt(typeTypeObj), canonTypeObj);
    }

    /*
     * properties
     */
    if (JsonSchemaGetField(interp, schemaObj, JsonAtomObjs[JSON_ATOM_PROPERTIES],
                                  NULL, &propertiesObj, NULL)) {
        if (JsonSchemaCanonicalizeProperties(interp, propertiesObj, &canonPropsObj) != NS_OK) {
            return NS_ERROR;
        }
        JsonTriplesAppendVt(outObj,
                            JsonAtomObjs[JSON_ATOM_PROPERTIES], JSON_VT_OBJECT, canonPropsObj);
    }

    /*
     * items
     */
    if (JsonSchemaGetField(interp, schemaObj, JsonAtomObjs[JSON_ATOM_ITEMS],
                                  NULL, &itemsObj, NULL)) {
        if (JsonSchemaCanonicalize(interp, itemsObj, &canonItemsObj) != NS_OK) {
            return NS_ERROR;
        }
        JsonTriplesAppendVt(outObj,
                            JsonAtomObjs[JSON_ATOM_ITEMS], JSON_VT_OBJECT, canonItemsObj);
    }

    /*
     * required
     */
    if (JsonSchemaGetField(interp, schemaObj, JsonAtomObjs[JSON_ATOM_REQUIRED],
                                  NULL, &requiredObj, NULL)) {
        TCL_SIZE_T roc;

        if (JsonSchemaCanonicalizeRequired(interp, requiredObj, &canonRequiredObj) != NS_OK) {
            return NS_ERROR;
        }
        if (Tcl_ListObjLength(interp, canonRequiredObj, &roc) != TCL_OK) {
            return NS_ERROR;
        }
        if (roc > 0) {
            JsonTriplesAppendVt(outObj,
                                JsonAtomObjs[JSON_ATOM_REQUIRED], JSON_VT_ARRAY, canonRequiredObj);
        }
    }

    /*
     * anyOf
     */
    if (JsonSchemaGetField(interp, schemaObj, JsonAtomObjs[JSON_ATOM_ANYOF],
                                  NULL, &anyOfObj, NULL)) {
        Tcl_Obj   **lv;
        TCL_SIZE_T  lc;

        if (Tcl_ListObjGetElements(interp, anyOfObj, &lc, &lv) != TCL_OK) {
            return NS_ERROR;
        }
        if ((lc % 3) != 0) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples schema: malformed anyOf array");
            return NS_ERROR;
        }

        canonAnyOfObj = Tcl_NewListObj(0, NULL);
        for (TCL_SIZE_T i = 0, idx = 0; i < lc; i += 3, idx++) {
            Tcl_Obj *canonAltObj = NULL;

            if (JsonTypeObjToVt(lv[i + 1]) != JSON_VT_OBJECT) {
                Ns_TclPrintfResult(interp,
                                   "ns_json triples schema: malformed anyOf array");
                return NS_ERROR;
            }
            if (JsonSchemaCanonicalize(interp, lv[i + 2], &canonAltObj) != NS_OK) {
                return NS_ERROR;
            }
            JsonTriplesAppendVt(canonAnyOfObj,
                                Tcl_NewWideIntObj((Tcl_WideInt)idx), JSON_VT_OBJECT, canonAltObj);
        }

        JsonTriplesAppendVt(outObj,
                            JsonAtomObjs[JSON_ATOM_ANYOF], JSON_VT_ARRAY, canonAnyOfObj);
    }

    *canonObjPtr = outObj;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaCanonicalizeTypeField --
 *
 *      Canonicalize the "type" field of a schema object.
 *
 *      The function normalizes the representation of the type
 *      specification so that scalar types and type unions appear in a
 *      stable and compact form.  For unions, the type names are ordered
 *      lexicographically to obtain deterministic output.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a Tcl object representing the canonicalized type
 *      field and stores it in *outTypeObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaCanonicalizeTypeField(Tcl_Interp *interp, Tcl_Obj *typeTypeObj, Tcl_Obj *typeValueObj,
                                Tcl_Obj **outTypeObjPtr)
{
    if (JsonTypeObjToVt(typeTypeObj) == JSON_VT_STRING) {
        *outTypeObjPtr = typeValueObj;
        return NS_OK;
    }

    return JsonSchemaCanonicalizeRequired(interp, typeValueObj, outTypeObjPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaCanonicalizeRequired --
 *
 *      Canonicalize the "required" field of a schema object.
 *
 *      The function normalizes the required-property list by sorting
 *      the property names lexicographically and removing duplicates in
 *      order to produce a stable representation.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a Tcl object containing the canonicalized array of
 *      property names and stores it in *canonRequiredObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaCanonicalizeRequired(Tcl_Interp *interp, Tcl_Obj *requiredObj,
                               Tcl_Obj **canonRequiredObjPtr)
{
    Tcl_Obj         **lv;
    TCL_SIZE_T        lc, nentries;
    JsonStringEntry  *entries;
    Tcl_Obj          *outObj;

    *canonRequiredObjPtr = NULL;

    if (Tcl_ListObjGetElements(interp, requiredObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed required array");
        return NS_ERROR;
    }

    nentries = lc / 3;
    entries = (JsonStringEntry *)ns_malloc((size_t)nentries * sizeof(JsonStringEntry));

    for (TCL_SIZE_T i = 0, j = 0; i < lc; i += 3, j++) {
        entries[j].valueObj = lv[i + 2];
    }

    qsort(entries, (size_t)nentries, sizeof(JsonStringEntry), JsonSchemaStringEntryCmp);

    outObj = Tcl_NewListObj(0, NULL);
    for (TCL_SIZE_T i = 0; i < nentries; i++) {
        JsonTriplesAppendVt(outObj,
                            Tcl_NewWideIntObj((Tcl_WideInt)i), JSON_VT_STRING, entries[i].valueObj);
    }

    ns_free(entries);
    *canonRequiredObjPtr = outObj;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaCanonicalizeProperties --
 *
 *      Canonicalize the "properties" field of a schema object.
 *
 *      The function sorts the property entries by name and recursively
 *      canonicalizes the schema associated with each property in order
 *      to obtain a deterministic object layout.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a Tcl object containing the canonicalized properties
 *      object and stores it in *canonPropsObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaCanonicalizeProperties(Tcl_Interp *interp, Tcl_Obj *propertiesObj,
                                 Tcl_Obj **canonPropsObjPtr)
{
    Tcl_Obj         **lv;
    TCL_SIZE_T        lc;
    Tcl_Obj          *sortedPropsObj = NULL;
    Tcl_Obj          *outObj = Tcl_NewListObj(0, NULL);

    *canonPropsObjPtr = NULL;

    if (JsonTriplesSortObject(interp, propertiesObj, &sortedPropsObj) != NS_OK) {
        return NS_ERROR;
    }
    if (Tcl_ListObjGetElements(interp, sortedPropsObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        Tcl_Obj *nameObj = lv[i + 0];
        Tcl_Obj *typeObj = lv[i + 1];
        Tcl_Obj *valueObj = lv[i + 2];
        Tcl_Obj *canonChildObj = NULL;

        if (JsonTypeObjToVt(typeObj) != JSON_VT_OBJECT) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples schema: malformed properties object");
            return NS_ERROR;
        }
        if (JsonSchemaCanonicalize(interp, valueObj, &canonChildObj) != NS_OK) {
            return NS_ERROR;
        }
        JsonTriplesAppendVt(outObj,
                            nameObj, JSON_VT_OBJECT, canonChildObj);
    }

    *canonPropsObjPtr = outObj;
    return NS_OK;
}

/*======================================================================
 * Function Implementations: Schema validation/matching helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMisMatchGetPath --
 *
 *      Prepend "/" to the path string, when it is emtpy.
 *
 * Results:
 *     JSON Pointer string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
JsonSchemaMisMatchGetPath(Tcl_DString *pathDsPtr)
{
    return pathDsPtr->length > 0 ? pathDsPtr->string : "/";
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaRequireSupportedSubset --
 *
 *      Verify that the provided schema uses only the subset of JSON
 *      Schema supported by the triples matcher.
 *
 *      The function performs a structural validation of the schema
 *      dictionary before it is used for matching.  It ensures that the
 *      schema node is a dictionary and that only supported schema
 *      keywords are present.  Nested schema objects occurring in
 *      "properties", "items", and "anyOf" are validated recursively.
 *
 *      The currently supported subset corresponds to the schemata
 *      generated by "ns_json triples schema".  This includes the
 *      keywords:
 *
 *          type
 *          properties
 *          items
 *          required
 *          anyOf
 *
 *      Metadata keys such as "$schema", "title", or "description"
 *      are accepted but ignored by the matcher.
 *
 *      When the flag ignoreUnsupported is false, encountering an
 *      unsupported keyword causes an error.  When the flag is true,
 *      unsupported keywords are silently ignored.
 *
 * Results:
 *      NS_OK if the schema conforms to the supported subset or if
 *      unsupported keywords are ignored.
 *
 *      NS_ERROR if the schema is malformed (e.g., wrong container
 *      types) or contains unsupported keywords while strict checking
 *      is enabled.
 *
 * Side effects:
 *      On error, an explanatory message is left in the interpreter
 *      result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaRequireSupportedSubset(Tcl_Interp *interp, Tcl_Obj *schemaObj,
                                 bool ignoreUnsupported)
{
    Tcl_DictSearch search;
    Tcl_Obj       *keyObj, *valueObj;
    int            done;

    if (Tcl_DictObjFirst(interp, schemaObj, &search, &keyObj, &valueObj, &done) != TCL_OK) {
        Tcl_SetObjResult(interp,
                         Tcl_NewStringObj("ns_json: malformed schema: schema node must be an object", -1));
        return NS_ERROR;
    }

    while (!done) {

        if (JsonObjIsAtom(keyObj, JSON_ATOM_SCHEMA)
            || JsonObjIsAtom(keyObj, JSON_ATOM_TITLE)
            || JsonObjIsAtom(keyObj, JSON_ATOM_DESCRIPTION)
            || JsonObjIsAtom(keyObj, JSON_ATOM_DEFAULT)
            || JsonObjIsAtom(keyObj, JSON_ATOM_EXAMPLES)
            ) {
            /*
             * Accepted metadata keys.  These are ignored by the matcher.
             */

        } else if (JsonObjIsAtom(keyObj, JSON_ATOM_TYPE)) {
            /*
             * Type validity is checked later by JsonSchemaMatchType().
             */

        } else if (JsonObjIsAtom(keyObj, JSON_ATOM_REQUIRED)) {
            Tcl_Obj  **rv;
            TCL_SIZE_T rc, i;

            if (Tcl_ListObjGetElements(interp, valueObj, &rc, &rv) != TCL_OK) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("ns_json: malformed schema: required must be a list", -1));
                Tcl_DictObjDone(&search);
                return NS_ERROR;
            }
            for (i = 0; i < rc; i++) {
                /*
                 * Require string-like elements.
                 */
                if (Tcl_GetString(rv[i]) == NULL) {
                    Tcl_SetObjResult(interp,
                                     Tcl_NewStringObj("ns_json: malformed schema: required elements must be strings", -1));
                    Tcl_DictObjDone(&search);
                    return NS_ERROR;
                }
            }

        } else if (JsonObjIsAtom(keyObj, JSON_ATOM_PROPERTIES)) {
            Tcl_DictSearch psearch;
            Tcl_Obj       *pkeyObj, *pschemaObj;
            int            pdone;

            if (Tcl_DictObjFirst(interp, valueObj, &psearch, &pkeyObj, &pschemaObj, &pdone) != TCL_OK) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("ns_json: malformed schema: properties must be an object", -1));
                Tcl_DictObjDone(&search);
                return NS_ERROR;
            }

            while (!pdone) {
                if (JsonSchemaRequireSupportedSubset(interp, pschemaObj, ignoreUnsupported) != NS_OK) {
                    Tcl_DictObjDone(&psearch);
                    Tcl_DictObjDone(&search);
                    return NS_ERROR;
                }
                Tcl_DictObjNext(&psearch, &pkeyObj, &pschemaObj, &pdone);
            }
            Tcl_DictObjDone(&psearch);

        } else if (JsonObjIsAtom(keyObj, JSON_ATOM_ITEMS)) {
            if (JsonSchemaRequireSupportedSubset(interp, valueObj, ignoreUnsupported) != NS_OK) {
                Tcl_DictObjDone(&search);
                return NS_ERROR;
            }

        } else if (JsonObjIsAtom(keyObj, JSON_ATOM_ANYOF)) {
            Tcl_Obj  **ov;
            TCL_SIZE_T oc, i;

            if (Tcl_ListObjGetElements(interp, valueObj, &oc, &ov) != TCL_OK || oc == 0) {
                Tcl_SetObjResult(interp,
                                 Tcl_NewStringObj("ns_json: malformed schema: anyOf must be a non-empty list", -1));
                Tcl_DictObjDone(&search);
                return NS_ERROR;
            }
            for (i = 0; i < oc; i++) {
                if (JsonSchemaRequireSupportedSubset(interp, ov[i], ignoreUnsupported) != NS_OK) {
                    Tcl_DictObjDone(&search);
                    return NS_ERROR;
                }
            }

        } else {
            if (!ignoreUnsupported) {
                Ns_TclPrintfResult(interp, "ns_json: unsupported schema keyword \"%s\"", Tcl_GetString(keyObj));
                Tcl_DictObjDone(&search);
                return NS_ERROR;
            }
            /*
             * Unsupported keyword is ignored in permissive mode.
             */
        }

        Tcl_DictObjNext(&search, &keyObj, &valueObj, &done);
    }

    Tcl_DictObjDone(&search);
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaDictGet --
 *
 *      Retrieve a schema field from a schema dictionary using the
 *      atomized field name.
 *
 *      The function performs a dictionary lookup for the specified
 *      schema keyword and returns the associated value if present.
 *      When the field does not exist, NULL is returned.
 *
 *      This helper hides the Tcl dictionary lookup and provides a
 *      concise way to access schema fields such as "type", "properties",
 *      "items", or "anyOf".
 *
 * Results:
 *      Returns the Tcl object representing the field value, or NULL
 *      if the field is not present.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
JsonSchemaDictGet(Tcl_Obj *schemaObj, JsonAtom atom)
{
    Tcl_Obj *valObj = NULL;

    NS_NONNULL_ASSERT(schemaObj != NULL);

    if (Tcl_DictObjGet(NULL, schemaObj, JsonAtomObjs[atom], &valObj) != TCL_OK) {
        return NULL;
    }
    return valObj;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMatchValue --
 *
 *      Match a value against a schema node.
 *
 *      This function implements the main recursive dispatcher used by
 *      the triples schema matcher.  It evaluates the schema constraints
 *      that apply to the current value and delegates further checks to
 *      specialized helpers.
 *
 *      The following schema keywords are handled:
 *
 *          anyOf       try alternative schema branches
 *          type        verify the JSON value type
 *          properties  validate object members
 *          items       validate array elements
 *
 *      For container values (objects and arrays), the function calls
 *      JsonSchemaMatchObject() or JsonSchemaMatchArray() respectively
 *      to perform recursive validation of nested values.
 *
 * Results:
 *      NS_OK if the value conforms to the schema.
 *      NS_ERROR if the value violates the schema.
 *
 * Side effects:
 *      On mismatch, an explanatory error message is left in the
 *      interpreter result, including the JSON Pointer path stored in
 *      pathDsPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMatchValue(Tcl_Interp *interp, Tcl_Obj *schemaObj,
                     JsonValueType actualVt, Tcl_Obj *actualObj,
                     Tcl_DString *pathDsPtr)
{
    Tcl_Obj *typeObj = NULL, *anyOfObj = NULL;

    anyOfObj = JsonSchemaDictGet(schemaObj, JSON_ATOM_ANYOF);
    if (anyOfObj != NULL) {
        return JsonSchemaMatchAnyOf(interp, anyOfObj, actualVt, actualObj, pathDsPtr);
    }

    typeObj = JsonSchemaDictGet(schemaObj, JSON_ATOM_TYPE);
    if (typeObj != NULL) {
        if (JsonSchemaMatchType(interp, typeObj, actualVt, pathDsPtr) != NS_OK) {
            return NS_ERROR;
        }
    }

    switch (actualVt) {
    case JSON_VT_OBJECT:
        return JsonSchemaMatchObject(interp, schemaObj, actualObj, pathDsPtr);

    case JSON_VT_ARRAY:
        return JsonSchemaMatchArray(interp, schemaObj, actualObj, pathDsPtr);

    case JSON_VT_STRING:
    case JSON_VT_NUMBER:
    case JSON_VT_NULL:
    case JSON_VT_BOOL:
    case JSON_VT_AUTO:
    default:
        return NS_OK;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMatchType --
 *
 *      Verify that the actual value type satisfies the schema "type"
 *      constraint.
 *
 *      The schema may specify either a single type name or a list of
 *      type names (a type union).  The function checks whether the
 *      actual value type matches one of the permitted schema types.
 *
 * Results:
 *      NS_OK if the value type matches the schema type constraint.
 *      NS_ERROR if the type constraint is violated.
 *
 * Side effects:
 *      On mismatch, an explanatory error message is left in the
 *      interpreter result describing the expected and actual types.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMatchType(Tcl_Interp *interp, Tcl_Obj *typesObj,
                    JsonValueType actualVt, Tcl_DString *pathDsPtr)
{
    TCL_SIZE_T objc;

    if (Tcl_ListObjLength(NULL, typesObj, &objc) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed schema: invalid type field", -1));
        return NS_ERROR;
    }

    if (objc <= 1) {
        JsonValueType expectedVt = JsonTypeObjToVt(typesObj);

        if (expectedVt != actualVt) {
            return JsonSchemaMismatchType(interp, pathDsPtr,
                                          Tcl_GetString(typesObj),
                                          JsonTypeString(actualVt));
        }
        return NS_OK;
    }

    /*
     * Union case: typesObj is a list of type names.
     */
    {
        Tcl_Obj   **objv;
        TCL_SIZE_T  i;
        const char *actualName = JsonTypeString(actualVt);

        if (Tcl_ListObjGetElements(interp, typesObj, &objc, &objv) != TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed schema: invalid type field", -1));
            return NS_ERROR;
        }

        for (i = 0; i < objc; i++) {
            JsonValueType expectedVt = JsonTypeObjToVt(objv[i]);

            if (expectedVt == actualVt) {
                return NS_OK;
            }
        }

        return JsonSchemaMismatchTypeUnion(interp, pathDsPtr, typesObj, actualName);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMatchAnyOf --
 *
 *      Evaluate an "anyOf" schema constraint.
 *
 *      The function iterates over the schema branches contained in
 *      the "anyOf" array and attempts to match the value against each
 *      branch in turn.  The value is considered valid if any branch
 *      matches successfully.
 *
 * Results:
 *      NS_OK if at least one branch matches the value.
 *      NS_ERROR if none of the branches match.
 *
 * Side effects:
 *      On failure, an explanatory error message is left in the
 *      interpreter result indicating that the value does not satisfy
 *      any of the allowed alternatives.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMatchAnyOf(Tcl_Interp *interp, Tcl_Obj *anyOfObj,
                     JsonValueType actualVt, Tcl_Obj *actualObj,
                     Tcl_DString *pathDsPtr)
{
    Tcl_Obj  **ov;
    TCL_SIZE_T oc, i;
    Tcl_Obj   *savedResultObj;

    if (Tcl_ListObjGetElements(interp, anyOfObj, &oc, &ov) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed schema: invalid anyOf field", -1));
        return NS_ERROR;
    }
    if (oc == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed schema: empty anyOf field", -1));
        return NS_ERROR;
    }

    /*
     * Try each branch.  Suppress branch-specific diagnostics and report
     * a single mismatch if none matches.
     */
    savedResultObj = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(savedResultObj);

    for (i = 0; i < oc; i++) {
        Tcl_ResetResult(interp);

        if (JsonSchemaMatchValue(interp, ov[i], actualVt, actualObj, pathDsPtr) == NS_OK) {
            Tcl_DecrRefCount(savedResultObj);
            return NS_OK;
        }
    }

    Tcl_SetObjResult(interp, savedResultObj);
    Tcl_DecrRefCount(savedResultObj);

    return JsonSchemaMismatchAnyOf(interp, pathDsPtr, actualVt);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMatchObject --
 *
 *      Validate a triples object value against an object schema.
 *
 *      The function checks the schema constraints applicable to JSON
 *      objects.  In particular, it verifies that all required properties
 *      are present and that every object member defined in the triples
 *      value has a corresponding schema definition in "properties".
 *
 *      For each object member, the associated schema is retrieved and
 *      JsonSchemaMatchValue() is called recursively to validate the
 *      member value.
 *
 * Results:
 *      NS_OK if the object satisfies the schema constraints.
 *      NS_ERROR if a required property is missing, an unexpected
 *      property appears, or a nested value violates its schema.
 *
 * Side effects:
 *      On mismatch, an explanatory error message is left in the
 *      interpreter result containing the JSON Pointer path of the
 *      failing location.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMatchObject(Tcl_Interp *interp, Tcl_Obj *schemaObj,
                      Tcl_Obj *triplesObj, Tcl_DString *pathDsPtr)
{
    Tcl_Obj     *propertiesObj;
    Tcl_Obj     *requiredObj;
    Tcl_Obj    **ov;
    TCL_SIZE_T   oc, i;

    propertiesObj = JsonSchemaDictGet(schemaObj, JSON_ATOM_PROPERTIES);
    requiredObj   = JsonSchemaDictGet(schemaObj, JSON_ATOM_REQUIRED);

    if (Tcl_ListObjGetElements(interp, triplesObj, &oc, &ov) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed triples object", -1));
        return NS_ERROR;
    }
    if ((oc % 3) != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed triples object", -1));
        return NS_ERROR;
    }

    /*
     * Check required properties.
     */
    if (requiredObj != NULL) {
        Tcl_Obj   **rv;
        TCL_SIZE_T  rc, j;

        if (Tcl_ListObjGetElements(interp, requiredObj, &rc, &rv) != TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed schema: invalid required field", -1));
            return NS_ERROR;
        }

        for (j = 0; j < rc; j++) {
            Tcl_Obj *requiredNameObj = rv[j];
            bool     found = NS_FALSE;

            for (i = 0; i < oc; i += 3) {
                if (TripleKeyMatches(ov[i], requiredNameObj)) {
                    found = NS_TRUE;
                    break;
                }
            }
            if (!found) {
                return JsonSchemaMismatchMissingRequired(interp, pathDsPtr, requiredNameObj);
            }
        }
    }

    /*
     * Check actual properties.
     */
    for (i = 0; i < oc; i += 3) {
        Tcl_Obj       *nameObj  = ov[i];
        Tcl_Obj       *typeObj  = ov[i + 1];
        Tcl_Obj       *valueObj = ov[i + 2];
        Tcl_Obj       *subschemaObj = NULL;
        JsonValueType  childVt;
        TCL_SIZE_T     oldLen;

        if (propertiesObj == NULL) {
            oldLen = Tcl_DStringLength(pathDsPtr);
            JsonPointerPathPushKey(pathDsPtr, nameObj);
            JsonSchemaMismatchUnexpectedProperty(interp, pathDsPtr);
            JsonPointerPathPop(pathDsPtr, oldLen);
            return NS_ERROR;
        }

        if (Tcl_DictObjGet(interp, propertiesObj, nameObj, &subschemaObj) != TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed schema: invalid properties field", -1));
            return NS_ERROR;
        }
        if (subschemaObj == NULL) {
            oldLen = Tcl_DStringLength(pathDsPtr);
            JsonPointerPathPushKey(pathDsPtr, nameObj);
            JsonSchemaMismatchUnexpectedProperty(interp, pathDsPtr);
            JsonPointerPathPop(pathDsPtr, oldLen);
            return NS_ERROR;
        }

        childVt = JsonTypeObjToVt(typeObj);

        oldLen = Tcl_DStringLength(pathDsPtr);
        JsonPointerPathPushKey(pathDsPtr, nameObj);

        if (JsonSchemaMatchValue(interp, subschemaObj, childVt, valueObj, pathDsPtr) != NS_OK) {
            JsonPointerPathPop(pathDsPtr, oldLen);
            return NS_ERROR;
        }

        JsonPointerPathPop(pathDsPtr, oldLen);
    }

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMatchArray --
 *
 *      Validate a triples array value against an array schema.
 *
 *      The function applies the schema specified by the "items"
 *      keyword to every element of the array.  Each element is
 *      validated by recursively invoking JsonSchemaMatchValue().
 *
 * Results:
 *      NS_OK if all array elements satisfy the schema constraint.
 *      NS_ERROR if any element violates the schema.
 *
 * Side effects:
 *      On mismatch, an explanatory error message is left in the
 *      interpreter result containing the JSON Pointer path of the
 *      failing array element.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMatchArray(Tcl_Interp *interp, Tcl_Obj *schemaObj,
                     Tcl_Obj *triplesObj, Tcl_DString *pathDsPtr)
{
    Tcl_Obj   *itemsObj;
    Tcl_Obj   **ov;
    TCL_SIZE_T  oc, i, idx;

    itemsObj = JsonSchemaDictGet(schemaObj, JSON_ATOM_ITEMS);
    if (itemsObj == NULL) {
        return NS_OK;
    }

    if (Tcl_ListObjGetElements(interp, triplesObj, &oc, &ov) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed triples array", -1));
        return NS_ERROR;
    }
    if ((oc % 3) != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("ns_json: malformed triples array", -1));
        return NS_ERROR;
    }

    for (i = 0, idx = 0; i < oc; i += 3, idx++) {
        Tcl_Obj       *typeObj  = ov[i + 1];
        Tcl_Obj       *valueObj = ov[i + 2];
        JsonValueType  childVt;
        TCL_SIZE_T     oldLen;

        childVt = JsonTypeObjToVt(typeObj);

        oldLen = Tcl_DStringLength(pathDsPtr);
        JsonPointerPathPushIndex(pathDsPtr, idx);

        if (JsonSchemaMatchValue(interp, itemsObj, childVt, valueObj, pathDsPtr) != NS_OK) {
            JsonPointerPathPop(pathDsPtr, oldLen);
            return NS_ERROR;
        }

        JsonPointerPathPop(pathDsPtr, oldLen);
    }

    return NS_OK;
}




/*======================================================================
 * Function Implementations: Schema mismatch reporting helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMismatchType --
 *
 *      Report a schema mismatch caused by an incorrect value type.
 *
 *      This helper formats an error message indicating that the actual
 *      JSON value type does not match the type required by the schema.
 *      The message includes the JSON Pointer path of the failing
 *      location.
 *
 * Results:
 *      Always returns NS_ERROR.
 *
 * Side effects:
 *      Leaves a descriptive error message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMismatchType(Tcl_Interp *interp, Tcl_DString *pathDsPtr,
                       const char *expectedType, const char *actualType)
{
    const char *path = JsonSchemaMisMatchGetPath(pathDsPtr);

    Ns_TclPrintfResult(interp,
                       "ns_json: schema mismatch at %s: expected %s, got %s",
                       path, expectedType, actualType);
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMismatchTypeUnion --
 *
 *      Report a schema mismatch for a type union constraint.
 *
 *      This helper is used when the schema specifies multiple allowed
 *      types (e.g., via a "type" list).  The function reports that the
 *      actual value type does not match any of the permitted types.
 *
 * Results:
 *      Always returns NS_ERROR.
 *
 * Side effects:
 *      Leaves a descriptive error message in the interpreter result,
 *      including the JSON Pointer path of the failing location.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMismatchTypeUnion(Tcl_Interp *interp, Tcl_DString *pathDsPtr,
                            Tcl_Obj *typeObj, const char *actualType)
{
    Tcl_DString expectedDs;
    Tcl_Obj   **ov;
    TCL_SIZE_T  oc, i;
    const char *path = JsonSchemaMisMatchGetPath(pathDsPtr);

    if (Tcl_ListObjGetElements(interp, typeObj, &oc, &ov) != TCL_OK || oc == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "ns_json: malformed schema: invalid type field", -1));
        return NS_ERROR;
    }

    Tcl_DStringInit(&expectedDs);
    Tcl_DStringAppend(&expectedDs, "[", 1);
    for (i = 0; i < oc; i++) {
        if (i > 0) {
            Tcl_DStringAppend(&expectedDs, ",", 1);
        }
        Tcl_DStringAppend(&expectedDs, Tcl_GetString(ov[i]), TCL_INDEX_NONE);
    }
    Tcl_DStringAppend(&expectedDs, "]", 1);

    Ns_TclPrintfResult(interp,
                       "ns_json: schema mismatch at %s: expected one of %s, got %s",
                       path, Tcl_DStringValue(&expectedDs), actualType);

    Tcl_DStringFree(&expectedDs);
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMismatchMissingRequired --
 *
 *      Report a missing required property in an object.
 *
 *      This helper formats an error message indicating that an object
 *      value does not contain a property listed in the schema's
 *      "required" constraint.
 *
 * Results:
 *      Always returns NS_ERROR.
 *
 * Side effects:
 *      Leaves a descriptive error message in the interpreter result,
 *      including the JSON Pointer path of the object where the
 *      required property is missing.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMismatchMissingRequired(Tcl_Interp *interp, Tcl_DString *pathDsPtr,
                                  Tcl_Obj *nameObj)
{
    const char *path = JsonSchemaMisMatchGetPath(pathDsPtr);

    Ns_TclPrintfResult(interp,
                       "ns_json: schema mismatch at %s: missing required property \"%s\"",
                       path, Tcl_GetString(nameObj));
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMismatchUnexpectedProperty --
 *
 *      Report an unexpected property in an object.
 *
 *      This helper is used when an object member appears that is not
 *      defined in the schema's "properties" section.
 *
 * Results:
 *      Always returns NS_ERROR.
 *
 * Side effects:
 *      Leaves a descriptive error message in the interpreter result,
 *      including the JSON Pointer path of the unexpected property.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMismatchUnexpectedProperty(Tcl_Interp *interp, Tcl_DString *pathDsPtr)
{
    const char *path = JsonSchemaMisMatchGetPath(pathDsPtr);

    Ns_TclPrintfResult(interp,
                       "ns_json: schema mismatch at %s: property not allowed by schema",
                       path);
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMismatchAnyOf --
 *
 *      Report a mismatch for an "anyOf" schema constraint.
 *
 *      This helper is used when none of the schema branches listed in
 *      an "anyOf" constraint match the provided value.
 *
 * Results:
 *      Always returns NS_ERROR.
 *
 * Side effects:
 *      Leaves a descriptive error message in the interpreter result
 *      indicating that the value does not satisfy any of the allowed
 *      alternatives at the specified JSON Pointer path.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMismatchAnyOf(Tcl_Interp *interp, Tcl_DString *pathDsPtr,
                        JsonValueType actualVt)
{
    const char *actualType;
    const char *path = JsonSchemaMisMatchGetPath(pathDsPtr);

    actualType = Tcl_GetString(JsonAtomObjs[actualVt]);
    Ns_TclPrintfResult(interp,
                       "ns_json: schema mismatch at %s: no anyOf alternative matched (got %s)",
                       path, actualType);

    return NS_ERROR;
}


/*======================================================================
 * Function Implementations: JSON Pointer path construction helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonPointerPathPushKey --
 *
 *      Append an object member name to the current JSON Pointer path.
 *
 *      The function extends the pointer stored in dsPtr by adding a
 *      slash followed by the escaped object key.  The key is encoded
 *      according to RFC 6901 using the same escaping rules as other
 *      pointer helpers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      dsPtr is extended with a new path segment representing the
 *      specified object key.
 *
 *----------------------------------------------------------------------
 */
static void
JsonPointerPathPushKey(Tcl_DString *dsPtr, Tcl_Obj *keyObj)
{
    const char *s;
    TCL_SIZE_T  len;

    s = Tcl_GetStringFromObj(keyObj, &len);
    Tcl_DStringAppend(dsPtr, "/", 1);
    JsonKeyPathEscapeSegment(dsPtr, s, len, NS_TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonPointerPathPushIndex --
 *
 *      Append an array index to the current JSON Pointer path.
 *
 *      The function extends the pointer stored in dsPtr by adding a
 *      slash followed by the decimal representation of the array index.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      dsPtr is extended with a new path segment representing the
 *      specified array index.
 *
 *----------------------------------------------------------------------
 */
static void
JsonPointerPathPushIndex(Tcl_DString *dsPtr, TCL_SIZE_T idx)
{
    JsonKeyPathAppendIndex(dsPtr, (size_t)idx);
}

/*
 *----------------------------------------------------------------------
 *
 * JsonPointerPathPop --
 *
 *      Restore the JSON Pointer path to a previous length.
 *
 *      This helper truncates the path stored in dsPtr to the length
 *      specified by oldLen.  It is typically used to undo a previous
 *      JsonPointerPathPushKey() or JsonPointerPathPushIndex() call
 *      after returning from recursive schema matching.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      dsPtr is shortened to the specified length.
 *
 *----------------------------------------------------------------------
 */
static void
JsonPointerPathPop(Tcl_DString *dsPtr, TCL_SIZE_T oldLen)
{
    if (oldLen < 0) {
        oldLen = 0;
    }
    Tcl_DStringSetLength(dsPtr, oldLen);
}


/*======================================================================
 * Function Implementations: Schema type helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaIsTypeOnlySchema --
 *
 *      Determine whether the provided schema object consists solely
 *      of a "type" specification.
 *
 *      The function checks whether schemaObj contains exactly one
 *      field named "type".  When this condition holds, the TYPE and
 *      VALUE objects of the triple are returned via the provided
 *      output pointers.
 *
 * Results:
 *      Returns NS_TRUE when schemaObj represents a type-only schema,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      When the schema is type-only, stores the TYPE and VALUE objects
 *      of the "type" field in the provided output pointers.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonSchemaIsTypeOnlySchema(Tcl_Interp *interp, Tcl_Obj *schemaObj,
                           Tcl_Obj **typeTypeObjPtr, Tcl_Obj **typeValueObjPtr)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    if (typeTypeObjPtr != NULL) {
        *typeTypeObjPtr = NULL;
    }
    if (typeValueObjPtr != NULL) {
        *typeValueObjPtr = NULL;
    }

    if (Tcl_ListObjGetElements(interp, schemaObj, &lc, &lv) != TCL_OK) {
        return NS_FALSE;
    }
    if (lc != 3) {
        return NS_FALSE;
    }
    if (!TripleKeyMatches(lv[0], JsonAtomObjs[JSON_ATOM_TYPE])) {
        return NS_FALSE;
    }

    /*
     * Allowed forms:
     *   type string <typename>
     *   type array  {0 string <typename> ...}
     */
    if (JsonTypeObjToVt(lv[1]) != JSON_VT_STRING
        && JsonTypeObjToVt(lv[1]) != JSON_VT_ARRAY) {
        return NS_FALSE;
    }

    if (typeTypeObjPtr != NULL) {
        *typeTypeObjPtr = lv[1];
    }
    if (typeValueObjPtr != NULL) {
        *typeValueObjPtr = lv[2];
    }
    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaExtractTypeUnion --
 *
 *      Extract the set of type names from the "type" field of a schema
 *      object.
 *
 *      The function retrieves the "type" specification from schemaObj
 *      and returns a normalized Tcl list containing the individual
 *      type names.  A scalar type is returned as a single-element list,
 *      while an array-valued type union is returned as-is.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a Tcl object containing the list of type names and
 *      stores it in *typesObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaExtractTypeUnion(Tcl_Interp *interp, Tcl_Obj *schemaObj, Tcl_Obj **typesObjPtr)
{
    Tcl_Obj *typeTypeObj = NULL, *typeValueObj = NULL;

    *typesObjPtr = NULL;

    if (!JsonSchemaIsTypeOnlySchema(interp, schemaObj, &typeTypeObj, &typeValueObj)) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: expected type-only schema");
        return NS_ERROR;
    }

    if (JsonTypeObjToVt(typeTypeObj) == JSON_VT_STRING) {
        Tcl_Obj *typesObj = Tcl_NewListObj(0, NULL);

        /*
         * Normalize:
         *   {type string number}
         * to:
         *   {0 string number}
         */
        JsonTriplesAppendVt(typesObj,
                            Tcl_NewWideIntObj(0), JSON_VT_STRING, typeValueObj);
        *typesObjPtr = typesObj;
        return NS_OK;
    }

    /*
     * Already a type union:
     *   {type array {0 string number 1 string null ...}}
     */
    *typesObjPtr = typeValueObj;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaTypeUnionContains --
 *
 *      Check whether a type union contains the specified type name.
 *
 *      The function scans the list of type names in typesObj and
 *      returns whether typeNameObj appears in the union.
 *
 * Results:
 *      Returns NS_TRUE when the type is present in the union,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonSchemaTypeUnionContains(Tcl_Interp *interp, Tcl_Obj *typesObj, Tcl_Obj *typeNameObj)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    if (Tcl_ListObjGetElements(interp, typesObj, &lc, &lv) != TCL_OK) {
        return NS_FALSE;
    }
    if ((lc % 3) != 0) {
        return NS_FALSE;
    }

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        if (JsonTypeObjToVt(lv[i + 1]) != JSON_VT_STRING) {
            continue;
        }
        if (strcmp(Tcl_GetString(lv[i + 2]), Tcl_GetString(typeNameObj)) == 0) {
            return NS_TRUE;
        }
    }
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaMergeTypeUnion --
 *
 *      Merge two sets of type names into a single union.
 *
 *      The function combines the type names from types1Obj and
 *      types2Obj, producing a merged list that contains each type
 *      at most once.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a Tcl object containing the merged type list and
 *      stores it in *mergedTypesObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaMergeTypeUnion(Tcl_Interp *interp, Tcl_Obj *types1Obj, Tcl_Obj *types2Obj,
                         Tcl_Obj **mergedTypesObjPtr)
{
    Tcl_Obj   *mergedObj;
    Tcl_Obj  **lv;
    TCL_SIZE_T lc, nextIdx;

    *mergedTypesObjPtr = NULL;

    mergedObj = Tcl_DuplicateObj(types1Obj);

    if (Tcl_ListObjGetElements(interp, mergedObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed type union");
        return NS_ERROR;
    }
    nextIdx = lc / 3;

    if (Tcl_ListObjGetElements(interp, types2Obj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed type union");
        return NS_ERROR;
    }

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        Tcl_Obj *typeNameObj = lv[i + 2];

        if (!JsonSchemaTypeUnionContains(interp, mergedObj, typeNameObj)) {
            JsonTriplesAppendVt(mergedObj,
                                Tcl_NewWideIntObj((Tcl_WideInt)nextIdx), JSON_VT_STRING, typeNameObj);
            nextIdx++;
        }
    }

    *mergedTypesObjPtr = mergedObj;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaBuildTypeUnion --
 *
 *      Build a schema fragment representing a union of JSON types.
 *
 *      The function constructs a triples-form schema object whose
 *      "type" field represents the provided list of type names.
 *      When the union contains a single type, the field is emitted as
 *      a scalar; otherwise it is emitted as an array of type names.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on failure.
 *
 * Side effects:
 *      Allocates a Tcl object representing the schema fragment and
 *      stores it in *schemaObjPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaBuildTypeUnion(Tcl_Interp *interp, Tcl_Obj *typesObj, Tcl_Obj **schemaObjPtr)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;
    Tcl_Obj    *schemaObj;

    *schemaObjPtr = NULL;

    if (Tcl_ListObjGetElements(interp, typesObj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0 || lc == 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed type union");
        return NS_ERROR;
    }

    schemaObj = Tcl_NewListObj(0, NULL);

    if (lc == 3) {
        /*
         * Compact back to:
         *   {type string X}
         */
        JsonTriplesAppendVt(schemaObj,
                            JsonAtomObjs[JSON_ATOM_TYPE], JSON_VT_STRING, lv[2]);
    } else {
        /*
         * Keep union form:
         *   {type array {0 string X 1 string Y ...}}
         */
        JsonTriplesAppendVt(schemaObj,
                            JsonAtomObjs[JSON_ATOM_TYPE], JSON_VT_ARRAY, typesObj);
    }

    *schemaObjPtr = schemaObj;
    return NS_OK;
}

/*----------------------------------------------------------------------
 *
 * JsonSchemaBuildAnyOf2 --
 *
 *      Build a minimal anyOf schema combining two schema fragments.
 *
 * Results:
 *      list of triples
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------*/
static Tcl_Obj *
JsonSchemaBuildAnyOf2(Tcl_Obj *schema1Obj, Tcl_Obj *schema2Obj)
{
    Tcl_Obj *schemaObj = Tcl_NewListObj(0, NULL);
    Tcl_Obj *anyOfObj  = Tcl_NewListObj(0, NULL);

    /*
     * anyOf is an array of schema objects
     */
    (void)JsonTriplesAppendVt(anyOfObj,
                              Tcl_NewWideIntObj(0), JSON_VT_OBJECT, schema1Obj);
    (void)JsonTriplesAppendVt(anyOfObj,
                              Tcl_NewWideIntObj(1), JSON_VT_OBJECT, schema2Obj);

    /*
     * schemaObj = { anyOf array anyOfObj }
     */
    (void)JsonTriplesAppendVt(schemaObj,
                              JsonAtomObjs[JSON_ATOM_ANYOF], JSON_VT_ARRAY, anyOfObj);

    return schemaObj;
}

/*======================================================================
 * Function Implementations: Schema required-field helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaRequiredIntersection --
 *
 *      Compute the intersection of two "required" property lists.
 *
 *      This helper is used during schema merging when the -required
 *      option is active.  The function determines which property names
 *      occur in both input "required" arrays and returns a new array
 *      containing only these common elements.
 *
 *      The order of elements in the resulting list follows the order
 *      of the first input list.  Duplicate entries are not introduced.
 *
 * Results:
 *      NS_OK on success.  The resulting intersection list is stored in
 *      *requiredOutObjPtr.
 *
 *      NS_ERROR on failure (e.g., malformed input).
 *
 * Side effects:
 *      A new Tcl list object is created and returned via
 *      *requiredOutObjPtr.  On error, an explanatory message is left in
 *      the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonSchemaRequiredIntersection(Tcl_Interp *interp, Tcl_Obj *required1Obj, Tcl_Obj *required2Obj,
                               Tcl_Obj **requiredOutObjPtr)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;
    Tcl_Obj    *outObj;
    TCL_SIZE_T  idx = 0;

    *requiredOutObjPtr = NULL;

    if (required1Obj == NULL || required2Obj == NULL) {
        /*
         * Intersection with missing required-set is empty.
         */
        *requiredOutObjPtr = Tcl_NewListObj(0, NULL);
        return NS_OK;
    }

    if (Tcl_ListObjGetElements(interp, required1Obj, &lc, &lv) != TCL_OK) {
        return NS_ERROR;
    }
    if ((lc % 3) != 0) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: malformed required array");
        return NS_ERROR;
    }

    outObj = Tcl_NewListObj(0, NULL);

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        Tcl_Obj *nameObj = lv[i + 2];

        if (JsonTypeObjToVt(lv[i + 1]) != JSON_VT_STRING) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples schema: malformed required array");
            return NS_ERROR;
        }

        if (JsonSchemaRequiredContains(interp, required2Obj, nameObj)) {
            JsonTriplesAppendVt(outObj,
                                Tcl_NewWideIntObj((Tcl_WideInt)idx), JSON_VT_STRING, nameObj);
            idx++;
        }
    }

    *requiredOutObjPtr = outObj;
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonSchemaRequiredContains --
 *
 *      Determine whether the specified property name occurs in the
 *      "required" array of a schema object.
 *
 *      The function scans the list of property names contained in
 *      requiredObj and checks whether nameObj is present.
 *
 * Results:
 *      Returns NS_TRUE when the property name is contained in the
 *      required list, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonSchemaRequiredContains(Tcl_Interp *interp, Tcl_Obj *requiredObj, Tcl_Obj *nameObj)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    if (Tcl_ListObjGetElements(interp, requiredObj, &lc, &lv) != TCL_OK) {
        return NS_FALSE;
    }
    if ((lc % 3) != 0) {
        return NS_FALSE;
    }

    for (TCL_SIZE_T i = 0; i < lc; i += 3) {
        if (JsonTypeObjToVt(lv[i + 1]) != JSON_VT_STRING) {
            continue;
        }
        if (strcmp(Tcl_GetString(lv[i + 2]), Tcl_GetString(nameObj)) == 0) {
            return NS_TRUE;
        }
    }
    return NS_FALSE;
}


/*======================================================================
 * Function Implementations: Result post-processing helpers.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * JsonMaybeWrapScanResult --
 *
 *      Helper for the Tcl-level interface to implement scan mode.  When
 *      isScan is true, wrap the current interpreter result into a two-element
 *      list of the form "{value bytes_consumed}".  When isScan is false, leave
 *      the interpreter result unchanged.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on Tcl allocation/list construction errors.
 *
 * Side Effects:
 *      May replace the interpreter result with a list containing the previous
 *      result object and the consumed byte count.
 *
 *----------------------------------------------------------------------
 */
static int
JsonMaybeWrapScanResult(Tcl_Interp *interp, bool isScan, size_t consumed)
{
    if (isScan) {
        Tcl_Obj *v = Tcl_GetObjResult(interp);
        Tcl_Obj *lv[2] = { v, Tcl_NewWideIntObj((Tcl_WideInt)consumed) };
        Tcl_SetObjResult(interp, Tcl_NewListObj(2, lv));
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonCheckTrailingDecode --
 *
 *      For non-scan parsing, verify that the input contains no trailing
 *      non-whitespace bytes after the parsed JSON value.  On trailing data,
 *      set a Tcl error describing the byte offset of the extra data.
 *
 * Results:
 *      TCL_OK when no trailing non-whitespace data is present, TCL_ERROR
 *      otherwise.
 *
 * Side Effects:
 *      On error, sets the interpreter result to an error message.
 *
 *----------------------------------------------------------------------
 */
static int
JsonCheckTrailingDecode(Tcl_Interp *interp,
                        const unsigned char *buf, size_t len, size_t consumed)
{
    const unsigned char *end = buf + len;
    const unsigned char *p;

    p = buf + consumed;
    p = JsonSkipWsPtr(p, end);

    if (p != end) {
        Ns_TclPrintfResult(interp,
                           "ns_json parse: trailing data at byte %lu",
                           (unsigned long)(p - buf));
        return TCL_ERROR;
    }
    return TCL_OK;
}


/*======================================================================
 * Function Implementations: Public API and registered Tcl cmds.
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * Ns_JsonParse --
 *
 *      Parse a JSON value from the provided byte buffer according to the
 *      supplied options and return the result in the requested representation.
 *      The function parses exactly one JSON value starting at buf[0] and
 *      reports how many bytes were consumed.
 *
 *      Depending on the selected output mode, the parsed value is returned
 *      either as a Tcl object (*resultObjPtr), as a flattened Ns_Set
 *      (*setPtr), or as a triples representation.  Only one of
 *      resultObjPtr or setPtr is expected to be non-NULL.
 *
 * Results:
 *      NS_OK on successful parse.
 *      NS_ERROR on parse error, with a human-readable message appended to
 *      errDsPtr.
 *
 * Side Effects:
 *      Advances the internal parser cursor while parsing.  On success,
 *      *consumedPtr is set to the number of bytes consumed from the input.
 *      On error, errDsPtr is populated with an error description.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_JsonParse(const unsigned char *buf, size_t len,
             const Ns_JsonOptions *opt,
             Tcl_Obj **resultObjPtr,
             Ns_Set *setPtr,
             size_t *consumedPtr,
             Tcl_DString *errDsPtr)
{
    JsonParser    jp;
    Tcl_Obj      *valueObj = NULL;
    JsonValueType vt = JSON_VT_AUTO;
    Ns_ReturnCode status = NS_ERROR;

    NS_NONNULL_ASSERT(buf != NULL);
    NS_NONNULL_ASSERT(opt != NULL);
    NS_NONNULL_ASSERT(consumedPtr != NULL);
    NS_NONNULL_ASSERT(errDsPtr != NULL);

    //NS_INIT_ONCE(NsJsonInitAtoms);

    if (resultObjPtr != NULL) {
        *resultObjPtr = NULL;
    }

    memset(&jp, 0, sizeof(jp));
    jp.interp   = NULL; /* not needed but maybe useful in the future */
    jp.start    = buf;
    jp.p        = buf;
    jp.end      = buf + len;
    jp.opt      = opt;
    jp.depth    = 0;
    jp.errDsPtr = errDsPtr;

    Tcl_DStringInit(&jp.tmpDs);
    Tcl_InitCustomHashTable(&jp.keyTable, TCL_CUSTOM_PTR_KEYS, &JsonKeyType);

    JsonSkipWs(&jp);

    if (jp.p >= jp.end) {
        Ns_DStringPrintf(jp.errDsPtr,
                         "ns_json: parse error at byte %lu: unexpected end of input",
                         (unsigned long)(jp.p - jp.start));
        goto fail;
    }

    /*
     * Top-level constraint (beginner guardrail).
     */
    if (opt->top == NS_JSON_TOP_CONTAINER) {
        int c = JsonPeek(&jp);
        if (c != '{' && c != '[') {
            Ns_DStringPrintf(jp.errDsPtr,
                             "ns_json: parse error at byte %lu: top-level value must be object or array (-top container)",
                             (unsigned long)(jp.p - jp.start));
            goto fail;
        }
    }

    /*
     * ns_set output: flatten directly into an Ns_Set.
     */
    if (opt->output == NS_JSON_OUTPUT_NS_SET) {
        Tcl_DString pathDs, typeKeyDs;
        bool        empty = NS_FALSE;

        if (setPtr == NULL) {
            Ns_DStringPrintf(jp.errDsPtr, "ns_json: internal error: set output requires Ns_Set*");
            goto fail;
        }
        /*
         * For ns_set output, scalar top-level is not very useful. One can
         * enforce this by default via -top container at the command level.
         * If not enforced there, we at least keep behavior deterministic:
         * top-level scalars do not produce entries (empty path), but
         * parsing still succeeds.
         */

        Tcl_DStringInit(&pathDs);
        Tcl_DStringInit(&typeKeyDs);

        if (JsonParseValueSet(&jp, setPtr, &pathDs, &typeKeyDs, &vt, &empty) != NS_OK) {
            Tcl_DStringFree(&typeKeyDs);
            Tcl_DStringFree(&pathDs);
            goto fail;
        }
        /*Ns_Log(Notice, "typeKeyDs size %d avail %d pathDs size %d avail %d set size %ld maxsze %ld data size %d avail %d",
               typeKeyDs.length, typeKeyDs.spaceAvl,
               pathDs.length, pathDs.spaceAvl,
               setPtr->size, setPtr->maxSize,
               setPtr->data.length, setPtr->data.spaceAvl);*/

        Tcl_DStringFree(&typeKeyDs);
        Tcl_DStringFree(&pathDs);

        *consumedPtr = (size_t)(jp.p - jp.start);

        status = NS_OK;

    } else {

        /*
         * dict / triples output.
         */

        if (JsonParseValue(&jp, &valueObj, &vt) != NS_OK) {
            goto fail;
        }

        *consumedPtr = (size_t)(jp.p - jp.start);

        /*
         * Output shaping for triples:
         *   - Always return a root wrapper: "" TYPE VALUE
         *   - For object/array, VALUE is the element-triples list for that container
         *   - For scalars, VALUE is the scalar Tcl_Obj
         */
        if (opt->output == NS_JSON_OUTPUT_TRIPLES) {
            Tcl_Obj *lv[3];
            lv[0] = JsonAtomObjs[JSON_ATOM_EMPTY];
            lv[1] = JsonAtomObjs[vt];
            lv[2] = valueObj;
            *resultObjPtr = Tcl_NewListObj(3, lv);
        } else {
            *resultObjPtr = valueObj;
        }

        status = NS_OK;
    }

 fail:
    Tcl_DStringFree(&jp.tmpDs);
    Tcl_DeleteHashTable(&jp.keyTable);

#if NS_JSON_KEY_SHARING
    /*Ns_Log(Notice, "nKeyAlloc %ld nKeyAllocDropped %ld nKeyFree %ld nKeyReuse %ld nKeyObjIncr %ld nKeyObjDecr %ld",
           jp.nKeyAlloc, jp.nKeyAllocDropped, jp.nKeyFree,jp.nKeyReuse, jp.nKeyObjIncr, jp.nKeyObjDecr
           );*/
#endif
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonNullObjCmd --
 *
 *      Implements "ns_json null".
 *
 *      Return a Tcl value representing JSON null.  The returned object
 *      uses the dedicated JSON-null Tcl object type (JsonNullObjType)
 *      and serves as the canonical representation of JSON null within
 *      ns_json.
 *
 *      The string representation of this object defaults to "null", but
 *      null detection must rely on the object type rather than the string
 *      representation.  The helper command "ns_json isnull" can be used
 *      to test whether a Tcl value represents JSON null.
 *
 * Results:
 *      TCL_OK with the interpreter result set to the canonical JSON-null
 *      Tcl object.
 *
 * Side effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonNullObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING]);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonIsNullObjCmd --
 *
 *      Implements "ns_json isnull".
 *
 *      Determine whether the provided Tcl value is represented by the
 *      dedicated JSON-null Tcl object type used by ns_json.
 *
 *      This check is based on the Tcl object type, not on the string
 *      representation.  Therefore, plain Tcl strings such as "null",
 *      the empty string, or values produced via "-nullvalue" during
 *      parsing are not considered JSON null unless they are represented
 *      by the dedicated JSON-null object type.
 *
 * Results:
 *      TCL_OK with the interpreter result set to a boolean value (1 or 0),
 *      TCL_ERROR on wrong argument count or other Tcl-level errors.
 *
 * Side Effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonIsNullObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *valueObj;
    int         result = TCL_OK;
    Ns_ObjvSpec args[] = {
        {"value", Ns_ObjvObj, &valueObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(JsonIsNullObj(valueObj)));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyDecodeObjCmd --
 *
 *      Implements "ns_json keydecode".
 *
 *      Decode a single escaped key path segment produced by ns_json's set
 *      output mode.  This is the inverse of "ns_json keyencode" and converts
 *      the escape sequences back to their original characters.
 *
 * Results:
 *      TCL_OK on success with the decoded segment as interpreter result,
 *      TCL_ERROR on wrong argument count or other Tcl-level errors.
 *
 * Side Effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonKeyDecodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *sObj;
    const char *s;
    TCL_SIZE_T  len;
    Tcl_DString ds;

    Ns_ObjvSpec args[] = {
        {"string", Ns_ObjvObj, &sObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    s = Tcl_GetStringFromObj(sObj, &len);

    Tcl_DStringInit(&ds);
    JsonKeyPathUnescapeSegment(&ds, s, len, NS_FALSE);
    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyEncodeObjCmd --
 *
 *      Implements "ns_json keyencode".
 *
 *      Encode an arbitrary string into an escaped key path segment suitable
 *      for use in keys produced by ns_json's set output mode.  The escaping
 *      avoids ambiguity with path separators and sidecar fields.
 *
 * Results:
 *      TCL_OK on success with the encoded segment as interpreter result,
 *      TCL_ERROR on wrong argument count or other Tcl-level errors.
 *
 * Side Effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonKeyEncodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *sObj;
    const char *s;
    TCL_SIZE_T  len;
    Tcl_DString ds;

    Ns_ObjvSpec args[] = {
        {"string", Ns_ObjvObj, &sObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    s = Tcl_GetStringFromObj(sObj, &len);

    Tcl_DStringInit(&ds);
    JsonKeyPathEscapeSegment(&ds, s, len, NS_FALSE);
    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonKeyInfoObjCmd --
 *
 *      Implements "ns_json keyinfo".
 *
 *      Interpret a flattened key produced by ns_json's set output mode and
 *      return a dict describing the base key and optional sidecar field.
 *      The base key is returned in unescaped form (per segment), with any
 *      recognized sidecar suffix (e.g., ".type") removed.
 *
 * Results:
 *      TCL_OK on success with a dict result containing "key" and "field",
 *      TCL_ERROR on wrong argument count or other Tcl-level errors.
 *
 * Side Effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonKeyInfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                  TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *keyObj;
    const char *key;
    TCL_SIZE_T  keyLen, baseLen;
    const char *field = "";
    TCL_SIZE_T  fieldLen = 0;
    Tcl_DString outDs, segDs;
    Tcl_Obj    *dictObj;

    Ns_ObjvSpec args[] = {
        {"key", Ns_ObjvObj, &keyObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    key = Tcl_GetStringFromObj(keyObj, &keyLen);

    /*
     * Split off known sidecar suffix ".type".
     */
    baseLen = JsonKeySplitSidecarField(key, keyLen, &field, &fieldLen);

    Tcl_DStringInit(&outDs);
    Tcl_DStringInit(&segDs);

    /*
     * Unescape per segment (split on '/'), then re-join with '/'.
     */
    {
        const char *p = key;
        const char *end = key + baseLen;
        const char *segStart = p;

        while (p <= end) {
            if (p == end || *p == '/') {
                TCL_SIZE_T segLen = (TCL_SIZE_T)(p - segStart);

                Tcl_DStringSetLength(&segDs, 0);
                JsonKeyPathUnescapeSegment(&segDs, segStart, segLen, NS_FALSE);

                if (outDs.length > 0) {
                    Tcl_DStringAppend(&outDs, "/", 1);
                }
                Tcl_DStringAppend(&outDs, segDs.string, segDs.length);

                segStart = p + 1;
            }
            p++;
        }
    }

    dictObj = Tcl_NewDictObj();

    Tcl_DictObjPut(interp, dictObj,
                   JsonAtomObjs[JSON_ATOM_KEY],
                   DStringToObj(&outDs));

    Tcl_DictObjPut(interp, dictObj,
                   JsonAtomObjs[JSON_ATOM_FIELD],
                   Tcl_NewStringObj(field, fieldLen));

    Tcl_DStringFree(&segDs);

    Tcl_SetObjResult(interp, dictObj);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonParseObjCmd --
 *
 *      Implements "ns_json parse".
 *
 *      Parse a single JSON value from the provided Tcl object and return the
 *      result in the requested output format (dict, triples, or set).  When
 *      the -scan option is specified, the result is returned as a two-element
 *      list "{value bytes_consumed}" suitable for concatenated or embedded JSON
 *      streams.
 *
 * Results:
 *      TCL_OK on success with the interpreter result set to the parsed value
 *      (or scan list), TCL_ERROR on syntax or parse errors.
 *
 * Side Effects:
 *      Sets the interpreter result.  In -output set mode, creates an Ns_Set and
 *      enters it into the interpreter with dynamic lifetime.
 *
 *----------------------------------------------------------------------
 */
static int
JsonParseObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_JsonOptions opt;
    Tcl_Obj       *resultObj = NULL, *valueObj;
    Tcl_DString    errDs;
    int            result = TCL_OK, isScan = 0;
    size_t         consumed = 0;
    const unsigned char *buf;
    TCL_SIZE_T     len;

    Ns_ObjvSpec opts[] = {
        {"-output",          Ns_ObjvIndex, &opt.output,          outputFormats},
        {"-scan",            Ns_ObjvBool,  &isScan,              INT2PTR(NS_TRUE)},
        {"-top",             Ns_ObjvIndex, &opt.top,             topModes},
        {"-nullvalue",       Ns_ObjvObj,   &opt.nullValueObj,    NULL},
        {"-validatenumbers", Ns_ObjvBool,  &opt.validateNumbers, INT2PTR(NS_TRUE)},
        {"-maxdepth",        Ns_ObjvInt,   &opt.maxDepth,        &posIntRange1},
        {"-maxstring",       Ns_ObjvInt,   &opt.maxString,       &posIntRange0},
        {"-maxcontainer",    Ns_ObjvInt,   &opt.maxContainer,    &posIntRange0},
        {"--",  Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"value", Ns_ObjvObj, &valueObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    memset(&opt, 0, sizeof(opt));
    opt.output       = NS_JSON_OUTPUT_TCL_VALUE;
    //opt.utf8         = NS_JSON_UTF8_STRICT;
    opt.top          = NS_JSON_TOP_ANY;
    opt.nullValueObj = JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING];
    opt.maxDepth     = 1000;
    opt.maxString    = 0;     /* 0 == unlimited (for now) */
    opt.maxContainer = 0;     /* 0 == unlimited (for now) */

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (opt.nullValueObj != JsonAtomObjs[JSON_ATOM_VALUE_NULL_STRING]) {
        opt.nullValueObj = JsonNewNullObj(opt.nullValueObj);
    }

    buf = (const unsigned char *)Tcl_GetStringFromObj(valueObj, &len);
    Tcl_DStringInit(&errDs);

    if (opt.output == NS_JSON_OUTPUT_NS_SET) {
        Ns_Set *setPtr = Ns_SetCreate("ns_json");

        if (Ns_JsonParse(buf, (size_t)len, &opt, NULL, setPtr, &consumed, &errDs) != NS_OK) {
            Ns_SetFree(setPtr);
            Tcl_DStringResult(interp, &errDs);
            result = TCL_ERROR;
            goto done;
        }

        if (!isScan) {
            if (JsonCheckTrailingDecode(interp, buf, (size_t)len, consumed) != TCL_OK) {
                Ns_SetFree(setPtr);
                result = TCL_ERROR;
                goto done;
            }
        }

        if (Ns_TclEnterSet(interp, setPtr, NS_TCL_SET_DYNAMIC) != TCL_OK) {
            Ns_SetFree(setPtr);
            result = TCL_ERROR;
            goto done;
        }

    } else {
        if (Ns_JsonParse(buf, (size_t)len, &opt, &resultObj, NULL, &consumed, &errDs) != NS_OK) {
            Tcl_DStringResult(interp, &errDs);
            result = TCL_ERROR;
            goto done;
        }

        Tcl_SetObjResult(interp, resultObj);

        if (!isScan) {
            if (JsonCheckTrailingDecode(interp, buf, (size_t)len, consumed) != TCL_OK) {
                result = TCL_ERROR;
                goto done;
            }
        }
    }

    /* Wrap whatever is currently in interp result */
    if (isScan) {
        (void) JsonMaybeWrapScanResult(interp, NS_TRUE, consumed);
    }

 done:
    Tcl_DStringFree(&errDs);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonValueObjCmd --
 *
 *      Implements "ns_json value".
 *
 *      Encode a single JSON value and return its JSON text representation.
 *      The optional -type argument controls how the Tcl value is interpreted
 *      (e.g., string, int, double, bool, null), and may also be used to
 *      serialize containers from triples input (-type object|array).
 *
 *      When -type auto (default) is selected, the command determines the JSON
 *      type using Tcl conversions in this order: number (integer, then double),
 *      then boolean, otherwise string.
 *
 * Results:
 *      TCL_OK on success with the generated JSON text as interpreter result,
 *      TCL_ERROR on wrong argument count, invalid options, or serialization
 *      errors.
 *
 * Side Effects:
 *      Sets the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonValueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj      *valueObj;
    JsonValueType vt = JSON_VT_AUTO;
    int           pretty = 0;
    Ns_ObjvSpec   opts[] = {
        {"-type",   Ns_ObjvIndex, &vt,     jsonValueTypes},
        {"-pretty", Ns_ObjvBool,  &pretty, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"value", Ns_ObjvObj, &valueObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else {
        Tcl_DString   ds, errDs;
        TCL_SIZE_T    len;
        const char   *s;

        Tcl_DStringInit(&ds);
        Tcl_DStringInit(&errDs);

        if (vt == JSON_VT_AUTO) {
            vt = JsonValueTypeDetect(interp, valueObj);
        }

        /*
         * If the input is a canonical root-wrapped triples document ("" TYPE VALUE),
         * unwrap it for encoding.  For AUTO, the wrapper TYPE wins.
         */
        {
            Tcl_Obj      *wTypeObj = NULL;
            Tcl_Obj      *wValueObj = NULL;
            JsonValueType wVt;

            wVt = TriplesDetectRootWrapper(interp, valueObj, &wTypeObj, &wValueObj);
            if (wVt != JSON_VT_AUTO) {
                if (vt == JSON_VT_AUTO) {
                    vt = wVt;
                }
                valueObj = wValueObj;
            }
        }

        switch (vt) {
        case JSON_VT_STRING:
            s = Tcl_GetStringFromObj(valueObj, &len);
            JsonAppendQuotedString(&ds, s, len);
            break;

        case JSON_VT_NUMBER: {
            Tcl_Obj *numObj;

            if (JsonNumberObjToLexeme(interp, valueObj, &numObj) != NS_OK) {
                return TCL_ERROR;
            }
            s = Tcl_GetStringFromObj(numObj, &len);
            Tcl_DStringAppend(&ds, s, len);
            break;
        }

        case JSON_VT_BOOL: {
            int b = 0;
            if (Tcl_GetBooleanFromObj(interp, valueObj, &b) != TCL_OK) {
                Tcl_DStringFree(&ds);
                return TCL_ERROR;
            }
            Tcl_DStringAppend(&ds, b ? "true" : "false", b ? 4 : 5);
            break;
        }

        case JSON_VT_NULL:
            Tcl_DStringAppend(&ds, "null", 4);
            break;

        case JSON_VT_OBJECT:
        case JSON_VT_ARRAY:
            {
                Tcl_Obj      *rootValue = NULL;
                JsonValueType rootVt;

                rootVt = TriplesDetectRootWrapper(interp, valueObj, NULL, &rootValue);

                if (vt != JSON_VT_AUTO) {
                    /*
                     * Explicit container type: accept either wrapped root or legacy
                     * container triples.
                     */
                    if (rootVt == vt) {
                        /* Wrapped root: serialize wrapped VALUE. */
                        valueObj = rootValue;

                    } else if (rootVt == JSON_VT_AUTO) {
                        /* No wrapper: treat valueObj itself as legacy container VALUE. */
                        /* nothing to do */

                    } else {
                        /* Wrapper present but mismatched type. */
                        Ns_TclPrintfResult(interp,
                                           "ns_json value: -type %s requires root-wrapped triples (\"\" %s VALUE)",
                                           (vt == JSON_VT_OBJECT ? "object" : "array"),
                                           (vt == JSON_VT_OBJECT ? "object" : "array"));
                        Tcl_DStringFree(&ds);
                        return TCL_ERROR;
                    }

                } else {
                    /*
                     * AUTO: accept wrapped container roots; otherwise fall back to
                     * legacy container detection (already in JsonValueTypeDetect()).
                     * Here, vt is already OBJECT/ARRAY from inference.
                     */
                    if (rootVt == JSON_VT_OBJECT || rootVt == JSON_VT_ARRAY) {
                        vt = rootVt;
                        valueObj = rootValue;
                    }
                }

                if (JsonTriplesRequireValidContainerObj(interp, valueObj, NS_TRUE, NS_TRUE, "value") != NS_OK
                    || JsonEmitContainerFromTriples(interp, valueObj, (vt == JSON_VT_OBJECT),
                                                    1, 0, pretty, &ds) != TCL_OK) {
                    Tcl_DStringFree(&ds);
                    return TCL_ERROR;
                }

                break;
            }

        case JSON_VT_AUTO:
            break;

        default:
            Ns_TclPrintfResult(interp, "ns_json value: unsupported type");
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }

        Tcl_DStringResult(interp, &ds);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesGetvalueObjCmd --
 *
 *      Implements "ns_json triples getvalue".
 *
 *      Parse arguments, resolve the requested path (via -path or -pointer),
 *      and return the value at that location from a triples structure.
 *      The default output format is JSON; with "-output triples" the command
 *      returns the underlying Tcl representation (scalar Tcl value or
 *      container-content triples list).  With "-indices" the command returns
 *      an index path suitable for use with lindex/lset on the original
 *      triples object.  The "-pretty" flag applies only to JSON output.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      On success, sets the interpreter result to the requested value or
 *      index path.  On error, leaves an explanatory message in the
 *      interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTriplesGetvalueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                          TCL_SIZE_T objc, Tcl_Obj *const* objv) {
    Tcl_Obj       *inputPathObj = NULL, *pointerObj = NULL, *triplesObj, *pathObj = NULL;
    int            wantIndex = 0, pretty = 0;
    JsonOutputMode outputMode = JSON_OUT_JSON;
    Ns_ObjvSpec    opts[] = {
        {"-path",    Ns_ObjvObj,   &inputPathObj, NULL},
        {"-pointer", Ns_ObjvObj,   &pointerObj,   NULL},
        {"-indices", Ns_ObjvBool,  &wantIndex,    INT2PTR(NS_TRUE)},
        {"-output",  Ns_ObjvIndex, &outputMode,   outputModeTable},
        {"-pretty",  Ns_ObjvBool,  &pretty,       INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"triples", Ns_ObjvObj, &triplesObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else if (outputMode == JSON_OUT_TRIPLES && pretty) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples getvalue: -pretty requires -output json");
        return TCL_ERROR;

    } else if (JsonTriplesGetPath(interp, inputPathObj, pointerObj, &pathObj) != NS_OK) {
        return TCL_ERROR;

    }

    if (wantIndex) {
        Tcl_Obj *idxPath = NULL;

        if (TriplesLookupPath(interp, pathObj, triplesObj, NULL, NULL, NULL, &idxPath, NULL) != NS_OK) {
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, idxPath);
    } else {
        Tcl_Obj *valueObj = NULL;
        JsonValueType vt;

        if (TriplesLookupPath(interp, pathObj, triplesObj, &valueObj, NULL, &vt, NULL, NULL) != NS_OK) {
            return TCL_ERROR;
        }
        if (outputMode == JSON_OUT_TRIPLES) {
            /*
             * For power users: raw scalar or container triples list.
             */
            Tcl_SetObjResult(interp, valueObj);

        } else {
            Tcl_Obj *jsonObj;

            if (JsonTriplesValueToJson(interp, vt, valueObj, (pretty != 0), NS_TRUE,
                                       &jsonObj) != TCL_OK) {
                return TCL_ERROR;
            }
            Tcl_SetObjResult(interp, jsonObj);
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesGettypeObjCmd --
 *
 *      Implements "ns_json triples gettype".
 *
 *      Parse arguments, resolve the requested path (via -path or -pointer),
 *      and return the JSON type token at that location from a triples
 *      structure.  With "-indices" the command returns an index path to the
 *      type slot suitable for use with lindex/lset on the original triples
 *      object.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      On success, sets the interpreter result to the type token or index
 *      path.  On error, leaves an explanatory message in the interpreter
 *      result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTriplesGettypeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                  TCL_SIZE_T objc, Tcl_Obj *const* objv) {
    Tcl_Obj *inputPathObj = NULL, *pointerObj = NULL, *triplesObj, *pathObj = NULL;
    int      wantIndex = 0;

    Ns_ObjvSpec opts[] = {
        {"-path",    Ns_ObjvObj,  &inputPathObj,    NULL},
        {"-pointer", Ns_ObjvObj,  &pointerObj,    NULL},
        {"-indices", Ns_ObjvBool, &wantIndex, INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"triples", Ns_ObjvObj, &triplesObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else if (JsonTriplesGetPath(interp, inputPathObj, pointerObj, &pathObj) != NS_OK) {
        return TCL_ERROR;
    }

    if (wantIndex) {
        Tcl_Obj *idxPath = NULL;

        if (TriplesLookupPath(interp, pathObj, triplesObj, NULL, NULL, NULL, NULL, &idxPath) != NS_OK) {
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, idxPath);
    } else {
        Tcl_Obj *typeObj = NULL;

        if (TriplesLookupPath(interp, pathObj, triplesObj, NULL, &typeObj, NULL, NULL, NULL) != NS_OK) {
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, typeObj);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesSetvalueObjCmd --
 *
 *      Implements "ns_json triples setvalue".
 *
 *      Parse arguments, resolve the requested path (via -path or -pointer),
 *      validate and normalize the provided value according to "-type", and
 *      update the triples structure at the specified location.
 *
 *      When "-type auto" is used, the command infers a type from the input
 *      while preserving and validating certain existing leaf types where
 *      applicable (e.g., number slots remain numeric and reject invalid
 *      number lexemes).  For explicit container types, the provided value
 *      must be a triples list representing the container content and is
 *      validated before committing the update.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      On success, sets the interpreter result to the updated triples list.
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTriplesSetvalueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                  TCL_SIZE_T objc, Tcl_Obj *const* objv) {
    Tcl_Obj      *inputPathObj = NULL, *pointerObj = NULL, *triplesObj, *pathObj = NULL;
    Tcl_Obj      *valueObj, *outObj, *outTriples = NULL;
    JsonValueType vt = JSON_VT_AUTO;
    Ns_ObjvSpec opts[] = {
        {"-path",    Ns_ObjvObj,   &inputPathObj, NULL},
        {"-pointer", Ns_ObjvObj,   &pointerObj,   NULL},
        {"-type",    Ns_ObjvIndex, &vt,           jsonValueTypes},
        {"--", Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"triples", Ns_ObjvObj, &triplesObj,  NULL},
        {"value",   Ns_ObjvObj, &valueObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else if (JsonTriplesGetPath(interp, inputPathObj, pointerObj, &pathObj) != NS_OK) {
        return TCL_ERROR;
    }

    outObj = valueObj;
    if (vt != JSON_VT_AUTO && JsonValidateValue(interp, vt, valueObj, &outObj, "triples setvalue") != NS_OK) {
        return TCL_ERROR;
    }
    if (TriplesSetValue(interp, pathObj, triplesObj, outObj, vt, &outTriples) != NS_OK) {
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, outTriples);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesSchemaObjCmd --
 *
 *      Implements "ns_json triples schema" subcommand.
 *
 *      The command derives a JSON Schema-like description from one or
 *      more JSON instances represented in canonical root-wrapped
 *      triples form.  For each input document a schema fragment is
 *      derived and the resulting schemas are merged into a single
 *      schema description.
 *
 *      When multiple instances are provided, the schemas are merged so
 *      that the resulting schema describes values compatible with all
 *      observed instances.  When the -required option is specified,
 *      required properties are computed using intersection semantics
 *      across the input documents.
 *
 * Results:
 *      TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      Sets the interpreter result to a JSON representation of the
 *      derived schema.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTriplesSchemaObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                        TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj      *rootTypeObj = NULL, *rootValueObj = NULL, *jsonObj = NULL;
    Tcl_Obj      *schemaObj = NULL, *canonSchemaObj = NULL;
    JsonValueType rootVt;
    TCL_SIZE_T    nargs = 0;
    int           pretty = 0, required = 0, result;
    Ns_ObjvSpec opts[] = {
        {"-pretty",   Ns_ObjvBool,  &pretty,   INT2PTR(NS_TRUE)},
        {"-required", Ns_ObjvBool,  &required, INT2PTR(NS_TRUE)},
        {"--",        Ns_ObjvBreak, NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"triples", Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    objv = objv + (objc - (TCL_SIZE_T)nargs);
    objc = (TCL_SIZE_T)nargs;

    for (TCL_SIZE_T i = 0; i < objc; i++) {
        Tcl_Obj *triplesObj = objv[i];
        Tcl_Obj *instanceSchemaObj = NULL;
        Tcl_Obj *mergedSchemaObj = NULL;

        rootVt = TriplesDetectRootWrapper(interp, triplesObj, &rootTypeObj, &rootValueObj);
        if (rootVt == JSON_VT_AUTO) {
            Ns_TclPrintfResult(interp,
                               "ns_json triples schema: triples document requires canonical root wrapper");
            return TCL_ERROR;
        }

        if (JsonSchemaFromValue(interp, rootVt, rootValueObj, (required != 0),
                                &instanceSchemaObj) != NS_OK) {
            return TCL_ERROR;
        }

        if (schemaObj == NULL) {
            schemaObj = instanceSchemaObj;
        } else {
            if (JsonSchemaMerge(interp, schemaObj, instanceSchemaObj, &mergedSchemaObj) != NS_OK) {
                return TCL_ERROR;
            }
            schemaObj = mergedSchemaObj;
        }
    }

    if (schemaObj == NULL) {
        Ns_TclPrintfResult(interp,
                           "ns_json triples schema: at least one triples document is required");
        return TCL_ERROR;
    }

    JsonTriplesAppendVt(schemaObj,
                        JsonAtomObjs[JSON_ATOM_SCHEMA], JSON_VT_STRING,
                        Tcl_NewStringObj("https://json-schema.org/draft/2020-12/schema", 44));

    if (JsonSchemaCanonicalize(interp, schemaObj, &canonSchemaObj) != NS_OK) {
        return TCL_ERROR;
    }
    schemaObj = canonSchemaObj;

    if (JsonValidateValue(interp, JSON_VT_OBJECT, schemaObj, &schemaObj,
                          "triples schema") != NS_OK) {
        return TCL_ERROR;
    }

    result = JsonTriplesValueToJson(interp, JSON_VT_OBJECT, schemaObj,
                                    (pretty != 0), NS_FALSE, &jsonObj);
    if (result == TCL_OK) {
        Tcl_SetObjResult(interp, jsonObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JsonTriplesMatchObjCmd --
 *
 *      Implements "ns_json triples match".
 *
 *      Parse arguments, parse the provided schema JSON into Tcl value
 *      form, validate that the schema uses the supported subset, detect
 *      the canonical root wrapper of the triples instance, and match the
 *      instance against the schema.
 *
 *      Matching is structural and type-based.  On success, the command
 *      returns 1.  On mismatch, it returns TCL_ERROR and leaves a
 *      diagnostic in the interpreter result, including a JSON Pointer
 *      path to the failing location.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      On success, sets the interpreter result to 1.
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTriplesMatchObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                       TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj      *schemaJsonObj = NULL, *triplesObj, *schemaObj = NULL;
    Tcl_Obj      *rootTypeObj = NULL, *rootValueObj = NULL;
    JsonValueType rootVt;
    Tcl_DString   pathDs;
    int           ignoreUnsupported = 0;
    Ns_ObjvSpec   opts[] = {
        {"!-schema",           Ns_ObjvObj,   &schemaJsonObj,     NULL},
        {"-ignoreunsupported", Ns_ObjvBool,  &ignoreUnsupported, INT2PTR(NS_TRUE)},
        {"--",                 Ns_ObjvBreak, NULL,               NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   args[] = {
        {"triples", Ns_ObjvObj, &triplesObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;

    } else {
        Ns_JsonOptions opt;
        Tcl_DString    errDs;
        TCL_SIZE_T     jsonLength;
        const char    *jsonString;
        size_t         consumed;

        /*
         * Parse schema JSON to Tcl value form ("tclvalue" semantics).
         */
        Tcl_DStringInit(&errDs);

        memset(&opt, 0, sizeof(opt));
        opt.output       = NS_JSON_OUTPUT_TCL_VALUE;
        opt.top          = NS_JSON_TOP_CONTAINER;     /* require { } or [ ] */
        opt.maxDepth     = 1000;
        jsonString = Tcl_GetStringFromObj(schemaJsonObj, &jsonLength);

        if (Ns_JsonParse((const unsigned char *)jsonString, (size_t)jsonLength, &opt,
                         &schemaObj, NULL, &consumed, &errDs) != NS_OK) {
            Tcl_DStringResult(interp, &errDs);
            return TCL_ERROR;
        }

        /*
         * Validate that the parsed schema uses only the supported subset:
         * $schema, type, properties, items, required, anyOf.
         */
        if (JsonSchemaRequireSupportedSubset(interp, schemaObj, ignoreUnsupported) != NS_OK) {
            return TCL_ERROR;
        }

        rootVt = TriplesDetectRootWrapper(interp, triplesObj, &rootTypeObj, &rootValueObj);
        if (rootVt == JSON_VT_AUTO) {
            return TCL_ERROR;
        }

        Tcl_DStringInit(&pathDs);

        if (JsonSchemaMatchValue(interp, schemaObj, rootVt, rootValueObj, &pathDs) != NS_OK) {
            Tcl_DStringFree(&pathDs);
            return TCL_ERROR;
        }

        Tcl_DStringFree(&pathDs);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        return TCL_OK;
    }
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclJsonObjCmd --
 *
 *      This command implements the "ns_json triples" command, dispatches the
 *      registered subcommands.
 *
 * Results:
 *      A standard Tcl result. TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *      Depends on the specific subcommand invoked.
 *
 *----------------------------------------------------------------------
 */
static int
JsonTriplesObjCmd(ClientData clientData, Tcl_Interp *interp,
                  TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"getvalue",  JsonTriplesGetvalueObjCmd},
        {"setvalue",  JsonTriplesSetvalueObjCmd},
        {"gettype",   JsonTriplesGettypeObjCmd},
        {"schema",    JsonTriplesSchemaObjCmd},
        {"match",     JsonTriplesMatchObjCmd},
        {NULL, NULL}
    };

    return Ns_SubsubcmdObjv(subcmds, clientData, interp, 1, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclJsonObjCmd --
 *
 *      This command implements the "ns_json" command, dispatches the
 *      registered subcommands.
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
NsTclJsonObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"isnull",     JsonIsNullObjCmd},
        {"null",       JsonNullObjCmd},
        {"keydecode",  JsonKeyDecodeObjCmd},
        {"keyencode",  JsonKeyEncodeObjCmd},
        {"keyinfo",    JsonKeyInfoObjCmd},
        {"parse",      JsonParseObjCmd},
        {"triples",    JsonTriplesObjCmd},
        {"value",      JsonValueObjCmd},
        {NULL, NULL}
    };
    //NS_INIT_ONCE(NsJsonInitAtoms);

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
