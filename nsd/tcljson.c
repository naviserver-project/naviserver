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

#define NS_JSON_NULL_SENTINEL "__NS_JSON_NULL__"
#define NS_JSON_NULL_SENTINEL_LEN (sizeof(NS_JSON_NULL_SENTINEL) - 1)

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
    JSON_ATOM_VALUE_NULL,

    JSON_ATOM_KEY,
    JSON_ATOM_FIELD,

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
    {-1, NS_JSON_NULL_SENTINEL, NS_JSON_NULL_SENTINEL_LEN}, /* JSON_ATOM_VALUE_NULL */

    /* misc */
    {-1, "key",   3},  /* JSON_ATOM_KEY */
    {-1, "field", 5}   /* JSON_ATOM_FIELD */
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
    {"dict",     NS_JSON_OUTPUT_DICT},
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
 * Local functions defined in this file
 */

static TCL_OBJCMDPROC_T JsonIsNullObjCmd;
static TCL_OBJCMDPROC_T JsonKeyDecodeObjCmd;
static TCL_OBJCMDPROC_T JsonKeyEncodeObjCmd;
static TCL_OBJCMDPROC_T JsonKeyInfoObjCmd;
static TCL_OBJCMDPROC_T JsonParseObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesGettypeObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesGetvalueObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesObjCmd;
static TCL_OBJCMDPROC_T JsonTriplesSetvalueObjCmd;
static TCL_OBJCMDPROC_T JsonValueObjCmd;

static Tcl_HashKeyProc JsonHashKeyProc;
static Tcl_CompareHashKeysProc JsonCompareKeysProc;

static Tcl_AllocHashEntryProc JsonAllocEntryProc;
static Tcl_FreeHashEntryProc JsonFreeEntryProc;

/*
 * Descriptor for custum hash table
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
static Tcl_Obj      *DStringToObj(Tcl_DString *dsPtr)  NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonAppendDecoded(JsonParser *jp, const unsigned char *at, const char *bytes, size_t len) NS_GNUC_NONNULL(1,2,3);
static void          JsonPrettyIndent(Tcl_DString *dsPtr, int depth) NS_GNUC_NONNULL(1);

static uint64_t      JsonEqByteMask(uint64_t x, uint64_t y) NS_GNUC_CONST;
static int           JsonWordAllWs(uint64_t w) NS_GNUC_CONST;
static uint64_t      JsonHasByteLt0x20(uint64_t x) NS_GNUC_CONST;
static Ns_ReturnCode JsonCheckNoCtlInString(JsonParser *jp, const unsigned char *p, const unsigned char *end) NS_GNUC_NONNULL(1,2,3);

static const unsigned char *JsonFindCtlLt0x20(const unsigned char *p, const unsigned char *end) NS_GNUC_NONNULL(1,2) NS_GNUC_PURE;
static const unsigned char *JsonSkipWsPtr(const unsigned char *p, const unsigned char *end) NS_GNUC_NONNULL(1,2);

static void          JsonSkipWs(JsonParser *jp) NS_GNUC_NONNULL(1);
static int           JsonPeek(JsonParser *jp) NS_GNUC_NONNULL(1);
static int           JsonGet(JsonParser *jp) NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonExpect(JsonParser *jp, int ch, const char *what) NS_GNUC_NONNULL(1,3);

static Ns_ReturnCode JsonDecodeHex4(JsonParser *jp, uint16_t *u16Ptr) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonDecodeUnicodeEscape(JsonParser *jp, uint32_t *cpPtr) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonScanNumber(JsonParser *jp, const unsigned char **startPtr,  const unsigned char **endPtr, bool *sawFracOrExpPtr)
    NS_GNUC_NONNULL(1,2,3,4);
static bool          JsonNumberLexemeIsValid(const unsigned char *s, size_t len) NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonValidateNumberString(const unsigned char *s, size_t len, Ns_DString *errDsPtr) NS_GNUC_NONNULL(1,3);

static Ns_ReturnCode JsonCheckStringSpan(JsonParser *jp, const unsigned char *p, const unsigned char *q, size_t outLen)
    NS_GNUC_NONNULL(1,2,3);

static JsonKey      *JsonKeyAlloc(JsonParser *jp, const char *bytes, TCL_SIZE_T len) NS_GNUC_NONNULL(2);
static Tcl_Obj      *JsonInternKeyObj(JsonParser *jp, const char *bytes, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);

static JsonValueType JsonTypeObjToVt(Tcl_Obj *typeObj) NS_GNUC_NONNULL(1);
static JsonValueType JsonInferValueType(Tcl_Interp *interp, Tcl_Obj *valueObj) NS_GNUC_NONNULL(1,2);
static bool          JsonTriplesIsPlausible(Tcl_Interp *interp, Tcl_Obj *valueObj) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonTriplesRequireValidContainerObj(Tcl_Interp *interp, Tcl_Obj *containerObj,
                                                         bool allowEmpty, bool validateNumbers,
                                                         const char *what) NS_GNUC_NONNULL(1,2,5);
static Ns_ReturnCode TriplesLookupPath(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *triplesObj,
                                       Tcl_Obj **valuePtr, Tcl_Obj **typePtr, JsonValueType *vtPtr,
                                       Tcl_Obj **valueIndexPathPtr, Tcl_Obj **typeIndexPathPtr) NS_GNUC_NONNULL(1,2,3);

static bool          TripleKeyMatches(Tcl_Obj *keyObj, Tcl_Obj *segObj)  NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode TriplesFind(Tcl_Interp *interp, Tcl_Obj *triplesObj, Tcl_Obj *segObj, TCL_SIZE_T *tripleBaseIdxPtr) NS_GNUC_NONNULL(1,2,3,4);

static bool          JsonIsNullObj(Tcl_Obj *valueObj) NS_GNUC_NONNULL(1);
static Ns_ReturnCode JsonRequireValidNumberObj(Tcl_Interp *interp, Tcl_Obj *valueObj) NS_GNUC_NONNULL(2);
static void          JsonTriplesDetectContainerType(Tcl_Interp *interp, Tcl_Obj *valueObj, JsonValueType *vtPtr) NS_GNUC_NONNULL(1,2,3);
static Tcl_Obj      *JsonPointerToPathObj(Tcl_Interp *interp, const char *p, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);

static Ns_ReturnCode JsonParseLiteral(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseNumber(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseStringToString(JsonParser *jp, const char **outPtr, TCL_SIZE_T *outLenPtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseString(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseKeyString(JsonParser *jp, Tcl_Obj **keyObjPtr) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonParseValue(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr) NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseValueSet(JsonParser *jp, Ns_Set *set, Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                       JsonValueType *valueTypePtr, bool *emptyPtr) NS_GNUC_NONNULL(1,2,3,4,5,6);

static Ns_ReturnCode JsonParseObject(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)  NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseObjectSet(JsonParser *jp, Ns_Set *set, Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                        bool *emptyPtr) NS_GNUC_NONNULL(1,2,3,4,5);

static Ns_ReturnCode JsonParseArray(JsonParser *jp, Tcl_Obj **valueObjPtr, JsonValueType *valueTypePtr)  NS_GNUC_NONNULL(1,2,3);
static Ns_ReturnCode JsonParseArraySet(JsonParser *jp, Ns_Set *set,
                                       Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                       bool *emptyPtr) NS_GNUC_NONNULL(1,2,3,4,5);

static Ns_ReturnCode JsonValidateNumberString(const unsigned char *s, size_t len, Ns_DString *errDsPtr) NS_GNUC_NONNULL(1,3);

static TCL_SIZE_T    JsonKeySplitSidecarField(const char *key, TCL_SIZE_T keyLen,
                                           const char **fieldPtr, TCL_SIZE_T *fieldLenPtr) NS_GNUC_NONNULL(1,3,4);
static void          JsonKeyPathEscapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen, bool rfc6901) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonTriplesGetPath(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *pointerObj, Tcl_Obj **outPathObj) NS_GNUC_NONNULL(1,4);
static Ns_ReturnCode TriplesSetValue(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *triplesObj, Tcl_Obj *newValueObj,
                                     JsonValueType vt, Tcl_Obj **resultTriplesPtr) NS_GNUC_NONNULL(1,2,3,4,6);

static Ns_ReturnCode JsonKeyPathUnescapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen, bool rfc6901) NS_GNUC_NONNULL(1,2);

static void          JsonKeyPathAppendSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen) NS_GNUC_NONNULL(1,2);
static void          JsonKeyPathAppendIndex(Tcl_DString *dsPtr, size_t idx) NS_GNUC_NONNULL(1);
static void          JsonKeyPathMakeTypeKey(Tcl_DString *typeKeyDsPtr, const char *key, TCL_SIZE_T keyLen) NS_GNUC_NONNULL(1,2);

static void          JsonSetPutValue(Ns_Set *set, const char *key, TCL_SIZE_T keyLen, const char *val, TCL_SIZE_T valLen) NS_GNUC_NONNULL(1,2,4);
static void          JsonSetPutType(Ns_Set *set, Tcl_DString *typeKeyDsPtr, const char *key, TCL_SIZE_T keyLen, JsonValueType vt) NS_GNUC_NONNULL(1,2,3);

static void          JsonAppendQuotedString(Tcl_DString *dsPtr, const char *s, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);
static void          JsonEmitTripleAppend(Tcl_Obj *listObj, Tcl_Obj *nameObj, Tcl_Obj *typeObj, Tcl_Obj *valueObj);
static int           JsonEmitValueFromTriple(Tcl_Interp *interp, Tcl_Obj *typeObj, Tcl_Obj *valObj,
                                             bool validateNumbers, int depth, bool pretty,
                                             Tcl_DString *dsPtr) NS_GNUC_NONNULL(1,2,3,7);
static int           JsonEmitContainerFromTriples(Tcl_Interp *interp, Tcl_Obj *triplesObj, bool isObject,
                                                  bool validateNumbers, int depth, bool pretty,
                                                  Tcl_DString *dsPtr) NS_GNUC_NONNULL(1,2,7);

static void          JsonFlattenToSet(Ns_Set *set, const Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                      Tcl_Obj *valueObj,  const char *valStr, TCL_SIZE_T valLen, JsonValueType vt) NS_GNUC_NONNULL(1,2,3);
static int           JsonMaybeWrapScanResult(Tcl_Interp *interp, bool isScan, size_t consumed) NS_GNUC_NONNULL(1);
static int           JsonCheckTrailingDecode(Tcl_Interp *interp, const unsigned char *buf, size_t len, size_t consumed) NS_GNUC_NONNULL(1,2);


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
}

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
 *        - returns non-zero when the keys differ
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
 * JsonIsNullObj --
 *
 *      Determine whether the provided Tcl object represents a JSON null
 *      value.
 *
 *      The function recognizes the canonical internal null atom as well as
 *      the textual null sentinel string used by the triples interface
 *      (e.g., "__NS_JSON_NULL__").  This allows callers to accept either
 *      representation without allocating new objects.
 *
 * Results:
 *      NS_TRUE if the value is the null atom or matches the null sentinel,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
JsonIsNullObj(Tcl_Obj *valueObj)
{
    bool success = NS_TRUE;

    if (valueObj != JsonAtomObjs[JSON_ATOM_VALUE_NULL]) {
        TCL_SIZE_T  n = 0;
        const char *s = Tcl_GetStringFromObj(valueObj, &n);

        success = ((size_t)n == NS_JSON_NULL_SENTINEL_LEN
                   && memcmp(s, NS_JSON_NULL_SENTINEL, NS_JSON_NULL_SENTINEL_LEN) == 0);
    }
    return success;
}

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
        *valueObjPtr = JsonAtomObjs[JSON_ATOM_VALUE_NULL];
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
            JsonEmitTripleAppend(accObj, keyObj, JsonAtomObjs[*valueTypePtr],
                                 valObj);
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

            JsonEmitTripleAppend(accObj, nameObj, JsonAtomObjs[*valueTypePtr],
                                 valObj);
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
     * NOTE: adjust field names to your JsonParser.
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

/*
 *----------------------------------------------------------------------
 *
 * JsonEmitTripleAppend --
 *
 *      Append one NAME TYPE VALUE triple to the given Tcl list object.
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
static void
JsonEmitTripleAppend(Tcl_Obj *listObj,
                     Tcl_Obj *nameObj,
                     Tcl_Obj *typeObj,
                     Tcl_Obj *valueObj)
{
    (void) Tcl_ListObjAppendElement(NULL, listObj, nameObj);
    (void) Tcl_ListObjAppendElement(NULL, listObj, typeObj);
    (void) Tcl_ListObjAppendElement(NULL, listObj, valueObj);
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

    if (*t == 's' && strcmp(t, "string") == 0) {
        const char *s; TCL_SIZE_T len;
        s = Tcl_GetStringFromObj(valObj, &len);
        JsonAppendQuotedString(dsPtr, s, len);
        return TCL_OK;

    } else if (*t == 'n' && strcmp(t, "number") == 0) {
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

    } else if (*t == 'b' && (strcmp(t, "bool") == 0 || strcmp(t, "boolean") == 0)) {
        int b = 0;
        if (Tcl_GetBooleanFromObj(interp, valObj, &b) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_DStringAppend(dsPtr, b ? "true" : "false", b ? 4 : 5);
        return TCL_OK;

    } else if (*t == 'n' && strcmp(t, "null") == 0) {
        Tcl_DStringAppend(dsPtr, "null", 4);
        return TCL_OK;

    } else if (*t == 'o' && strcmp(t, "object") == 0) {
        return JsonEmitContainerFromTriples(interp, valObj, NS_TRUE, validateNumbers, depth, pretty, dsPtr);

    } else if (*t == 'a' && strcmp(t, "array") == 0) {
        return JsonEmitContainerFromTriples(interp, valObj, NS_FALSE, validateNumbers, depth, pretty, dsPtr);

    } else {
        Ns_TclPrintfResult(interp, "ns_json: unsupported triple type \"%s\"", t);
        return TCL_ERROR;
    }
}

/*
 * Convert a (vt,valueObj) pair (as returned by triples navigation)
 * to JSON text.
 *
 *  * vt is the *node* type (string/number/boolean/null/object/array)
 *  * valueObj is:
 *    scalar -> Tcl_Obj containing scalar representation
 *    object/array -> Tcl list with container-content triples
 *
 * validateNumbers is expected to be NS_TRUE now (contract).
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
         * If you already have cached type atoms/objs (JsonAtomObjs[]),
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
    bool first = NS_TRUE;

    if (Tcl_ListObjGetElements(interp, triplesObj, &oc, &ov) != TCL_OK) {
        return TCL_ERROR;
    }
    if (oc % 3 != 0) {
        Ns_TclPrintfResult(interp, "ns_json: triples length must be multiple of 3");
        return TCL_ERROR;
    }

    Tcl_DStringAppend(dsPtr, isObject ? "{" : "[", 1);
    if (pretty && oc > 0) {
        JsonPrettyIndent(dsPtr, depth + 1);
    }
    for (i = 0; i < oc; i += 3) {
        Tcl_Obj    *nameObj = ov[i];
        Tcl_Obj    *typeObj = ov[i+1];
        Tcl_Obj    *valObj  = ov[i+2];

        if (!first) {
            Tcl_DStringAppend(dsPtr, ",", 1);
            if (pretty) {
                JsonPrettyIndent(dsPtr, depth + 1);
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
        JsonPrettyIndent(dsPtr, depth);
    }
    Tcl_DStringAppend(dsPtr, isObject ? "}" : "]", 1);
    return TCL_OK;
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
    JsonParser jp;
    Tcl_Obj   *valueObj = NULL;
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
    jp.interp   = NULL; /* not needed unless you want Tcl conversion helpers that require interp */
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
         *   - Top-level object/array: return element triples directly
         *   - Top-level scalar: return "" TYPE VALUE
         */
        if (opt->output == NS_JSON_OUTPUT_TRIPLES) {
            if (vt == JSON_VT_OBJECT || vt == JSON_VT_ARRAY) {
                /* valueObj already is the triples list for that container */
                *resultObjPtr = valueObj;
            } else {
                Tcl_Obj *lv[3];
                lv[0] = JsonAtomObjs[JSON_ATOM_EMPTY];
                lv[1] = JsonAtomObjs[vt];
                lv[2] = valueObj;
                *resultObjPtr = Tcl_NewListObj(3, lv);
            }
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

/*
 *----------------------------------------------------------------------
 *
 * JsonIsNullObjCmd --
 *
 *      Implements "ns_json isnull".
 *
 *      Determine whether the provided Tcl value is the distinguished JSON null
 *      sentinel used by ns_json in dict output mode to represent JSON null
 *      without ambiguity (i.e., distinct from the JSON string "null" or other
 *      empty values).
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
    opt.output       = NS_JSON_OUTPUT_DICT;
    //opt.utf8         = NS_JSON_UTF8_STRICT;
    opt.top          = NS_JSON_TOP_ANY;
    opt.maxDepth     = 1000;
    opt.maxString    = 0;     /* 0 == unlimited (for now) */
    opt.maxContainer = 0;     /* 0 == unlimited (for now) */

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
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
 *-----------------------------------------------------------------------
 *
 * JsonTriplesIsPlausible --
 *
 *      Check whether a Tcl object looks like a triples list, i.e. a Tcl list
 *      whose length is a multiple of three. This predicate is used as a cheap
 *      guard to avoid treating malformed lists as triples containers.
 *
 *      The check is intentionally conservative: it validates only list-ness
 *      and the length multiple-of-3 constraint. It does not validate per-triplet
 *      types or nested container structure.
 *
 * Results:
 *      NS_TRUE when valueObj is a list and its length is a multiple of 3,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------
 */
static bool
JsonTriplesIsPlausible(Tcl_Interp *interp, Tcl_Obj *valueObj)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    if (Tcl_ListObjGetElements(interp, valueObj, &lc, &lv) != TCL_OK) {
        return NS_FALSE;
    }
    return (lc != 0 && (lc % 3) == 0);
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
 * JsonTriplesDetectContainerType --
 *
 *      Determine whether a triples list represents an object container or
 *      an array container based on its keys.
 *
 *      The function inspects the "key-or-index" positions of the triples
 *      list.  If the entries appear to be numeric indices, the container
 *      is classified as an array; otherwise it is classified as an object.
 *      This is used only when a container type is needed but not explicitly
 *      provided by surrounding context.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      On return, *vtPtr is set to JSON_VT_OBJECT or JSON_VT_ARRAY.
 *      If the container type cannot be determined reliably, *vtPtr is set
 *      to JSON_VT_AUTO.
 *
 *----------------------------------------------------------------------
 */
static void
JsonTriplesDetectContainerType(Tcl_Interp *interp, Tcl_Obj *valueObj, JsonValueType *vtPtr)
{
    Tcl_Obj   **lv;
    TCL_SIZE_T  lc;

    *vtPtr = JSON_VT_AUTO;

    if (Tcl_ListObjGetElements(interp, valueObj, &lc, &lv) != TCL_OK) {
        return;
    }
    if (lc == 0 || (lc % 3) != 0) {
        return;
    }

    /*
     * Triples look like: {key|index type value} ...
     * Recognize only explicit container markers ("object" / "array") in the
     * first triple's type token.
     */
    {
        JsonValueType vt = JsonTypeObjToVt(lv[1]);

        if (vt == JSON_VT_OBJECT || vt == JSON_VT_ARRAY) {
            *vtPtr = JSON_VT_OBJECT;
        }
    }
}

/*
 *-----------------------------------------------------------------------
 *
 * JsonInferValueType --
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
JsonInferValueType(Tcl_Interp *interp, Tcl_Obj *valueObj)
{
    JsonValueType tvt = JSON_VT_AUTO;

    JsonTriplesDetectContainerType(interp, valueObj, &tvt);
    if (tvt != JSON_VT_AUTO) {
        if (JsonTriplesIsPlausible(interp, valueObj)) {
            return tvt;
        }
        /*
         * Ignore malformed/ambiguous container-looking inputs in AUTO.
         * The final fallback remains VT_STRING via scalar classification.
         */
    }

    /*
     * Conservative scalar AUTO: number, boolean, null sentinel, else string.
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
        if ((size_t)len == NS_JSON_NULL_SENTINEL_LEN
            && memcmp(s, NS_JSON_NULL_SENTINEL, NS_JSON_NULL_SENTINEL_LEN) == 0) {
            return JSON_VT_NULL;
        }
    }

    return JSON_VT_STRING;
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
 * JsonValidateValue --
 *
 *      Validate and optionally normalize a value according to a requested
 *      JSON value type.
 *
 *      For scalar types, this function checks that inObj is compatible with
 *      the requested type and, when applicable, produces a canonicalized
 *      representation in *outObjPtr (e.g., canonical boolean objects and the
 *      null sentinel).  For numbers, the function validates the JSON number
 *      lexeme.  For container types (object/array), it validates that inObj
 *      is a plausible triples list (length multiple of 3) and may perform
 *      additional checks depending on the configuration (e.g., number
 *      validation within the container).
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
        if (JsonRequireValidNumberObj(interp, inObj) != NS_OK) {
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
         * Normalize to the null sentinel (or a dedicated null atom if you have one).
         */
        outObj = JsonAtomObjs[JSON_ATOM_VALUE_NULL];
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
    int           validateNumbers = 1, pretty = 0;
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
            vt = JsonInferValueType(interp, valueObj);
        }

        switch (vt) {
        case JSON_VT_STRING:
            s = Tcl_GetStringFromObj(valueObj, &len);
            JsonAppendQuotedString(&ds, s, len);
            break;

        case JSON_VT_NUMBER:
            /*
             * all numeric-ish: emit as bytes (caller responsibility for number lexeme validity in -type number)
             */
            s = Tcl_GetStringFromObj(valueObj, &len);

            if (validateNumbers
                && (JsonValidateNumberString((const unsigned char *)s, (size_t)len, &errDs) != NS_OK)
                ) {
                Tcl_DStringResult(interp, &errDs);
                return TCL_ERROR;
            } else {
                Tcl_DStringAppend(&ds, s, len);
            }
            break;

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
            /*
             * Treat valueObj as triples list and serialize accordingly.
             * Call the same serializer you’ll implement for "ns_json triples".
             */

            if (JsonTriplesRequireValidContainerObj(interp, valueObj, NS_TRUE, NS_TRUE, "value") != NS_OK
                || JsonEmitContainerFromTriples(interp, valueObj, (vt == JSON_VT_OBJECT),
                                                validateNumbers, 0, pretty, &ds) != TCL_OK) {
                Tcl_DStringFree(&ds);
                return TCL_ERROR;
            }
            break;

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
#if 0
    {bool matches;

        matches = Tcl_StringMatch(k, s);
        Ns_Log(Notice, "compare k <%s> with seg <%s> -> matches %d", k, s, matches);
        return matches;
    }
#endif
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

/*
 *----------------------------------------------------------------------
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
 * Results:
 *      Tcl path list object on success, NULL on error.
 *
 * Side effects:
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
JsonPointerToPathObj(Tcl_Interp *interp, const char *p, TCL_SIZE_T len)
{
    Tcl_DString listDs, segDs;
    TCL_SIZE_T i, start;
    Tcl_Obj *pathObj = NULL;

    if (len < 0) {
        len = (TCL_SIZE_T)strlen(p);
    }

    /*
     * RFC 6901 JSON Pointer:
     * "" => whole document (map to empty path list)
     * "/a/b" => segments a, b
     */
    if (len == 0) {
        return Tcl_NewListObj(0, NULL);
    }
    if (p[0] != '/') {
        Ns_TclPrintfResult(interp,
                           "ns_json triples: invalid JSON pointer (must start with '/'): %s", p);
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
                                   "ns_json triples: invalid JSON pointer escape in: %s", p);
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
 *      When requested, valueIndexPathPtr and typeIndexPathPtr are set to Tcl
 *      index paths that address, respectively, the leaf value slot (base+2)
 *      and the leaf type slot (base+1) within the original triples list.
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

    if (valuePtr != NULL)         { *valuePtr = NULL; }
    if (typePtr != NULL)          { *typePtr = NULL; }
    if (vtPtr != NULL)            { *vtPtr = JSON_VT_AUTO; }
    if (valueIndexPathPtr != NULL){ *valueIndexPathPtr = NULL; }
    if (typeIndexPathPtr != NULL) { *typeIndexPathPtr = NULL; }

    if (Tcl_ListObjGetElements(interp, pathObj, &pc, &pv) != TCL_OK) {
        return NS_ERROR;
    }
    if (pc == 0) {
        Ns_TclPrintfResult(interp, "ns_json triples: empty path");
        return NS_ERROR;
    }

    vIndexPath = Tcl_NewListObj(0, NULL);
    tIndexPath = Tcl_NewListObj(0, NULL);

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
            if (vtPtr != NULL) { *vtPtr = JsonTypeObjToVt(lv[base + 1]); }

            (void)Tcl_ListObjAppendElement(interp, vIndexPath, Tcl_NewIntObj((int)(base + 2)));
            (void)Tcl_ListObjAppendElement(interp, tIndexPath, Tcl_NewIntObj((int)(base + 1)));

            if (valueIndexPathPtr != NULL) {
                *valueIndexPathPtr = vIndexPath;
            } else {
                Tcl_DecrRefCount(vIndexPath);
            }
            if (typeIndexPathPtr != NULL)  {
                *typeIndexPathPtr  = tIndexPath;
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
        /*
         * Descend into nested triples (value element).
         */
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
 * TriplesSetValue --
 *
 *      Update the value at the specified path within a triples structure and
 *      return a modified triples list.
 *
 *      The pathObj is a Tcl list of path segments.  The function resolves the
 *      path to the leaf element, applies type handling according to vt, and
 *      constructs an updated triples list in *resultTriplesPtr.
 *
 *      For scalar types, the newValueObj is stored as the leaf value after
 *      required validation/normalization has been performed by the caller or
 *      at commit time.  For container types (object/array), newValueObj must
 *      be a triples list representing the container content; the container
 *      content is validated (structure and, when enabled, nested number
 *      lexemes) before committing the update.
 *
 * Results:
 *      NS_OK on success and *resultTriplesPtr is set to the updated triples
 *      list, NS_ERROR otherwise.
 *
 * Side effects:
 *      On success, creates a new Tcl object for the updated triples list.
 *      On error, leaves an explanatory message in the interpreter result.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
TriplesSetValue(Tcl_Interp *interp, Tcl_Obj *pathObj, Tcl_Obj *triplesObj, Tcl_Obj *newValueObj, JsonValueType vt,
                Tcl_Obj **resultTriplesPtr)
{
    Tcl_Obj   **pv;
    TCL_SIZE_T  pc;

    if (Tcl_ListObjGetElements(interp, pathObj, &pc, &pv) != TCL_OK) {
        return NS_ERROR;
    }
    if (pc == 0) {
        Ns_TclPrintfResult(interp, "ns_json triples setvalue: empty path");
        return NS_ERROR;
    }

    /*
     * Recursive, duplicating rewrite.
     */
    {
        Tcl_Obj *out = Tcl_DuplicateObj(triplesObj);
        Tcl_Obj *cur = out;

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
                            Tcl_ResetResult(interp); /* since we used NULL interp above */
                        }
                    }
                    if (vt == JSON_VT_AUTO) {
                        vt = JsonInferValueType(interp, newValueObj);
                        JsonValidateValue(interp, vt, newValueObj, &newValueObj, "triples setvalue");
                    }
                } else {
                    if (vt == JSON_VT_NULL) {
                        newValueObj = JsonAtomObjs[JSON_ATOM_VALUE_NULL];
                    }
                }

                /*
                 * Replace the VALUE slot (base+2). Keep KEY and TYPE unchanged.
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
