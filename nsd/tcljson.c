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
#define SHARED_ATOMS 0

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


#if SHARED_ATOMS
# include "nsatoms.h"
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
} NsJsonAtom;

static const NsAtomSpec jsonAtomSpecs[JSON_ATOM_MAX] = {
    /* Keep indices aligned with NsJsonAtom enum order */

    /* JSON_ATOM_UNUSED (matches JSON_VT_AUTO) */
    {NS_ATOM__MAX, "auto", 4},

    /* type atoms (MUST align with JsonValueType) */
    {NS_ATOM_STRING,  NULL, 0},  /* JSON_ATOM_T_STRING */
    {NS_ATOM_NUMBER,  NULL, 0},  /* JSON_ATOM_T_NUMBER */
    {NS_ATOM_BOOLEAN, NULL, 0},  /* JSON_ATOM_T_BOOLEAN */
    {NS_ATOM_NULL,    NULL, 0},  /* JSON_ATOM_T_NULL */
    {NS_ATOM_OBJECT,  NULL, 0},  /* JSON_ATOM_T_OBJECT */
    {NS_ATOM_ARRAY,   NULL, 0},  /* JSON_ATOM_T_ARRAY */

    /* non-type atoms */
    {NS_ATOM_TRUE,  NULL, 0},          /* JSON_ATOM_TRUE */
    {NS_ATOM_FALSE, NULL, 0},          /* JSON_ATOM_FALSE */
    {NS_ATOM_EMPTY, NULL, 0},          /* JSON_ATOM_EMPTY */
    {NS_ATOM__MAX, NS_JSON_NULL_SENTINEL, NS_JSON_NULL_SENTINEL_LEN}, /* JSON_ATOM_VALUE_NULL */

    /* misc */
    {NS_ATOM__MAX, "key",   3},         /* JSON_ATOM_KEY */
    {NS_ATOM__MAX, "field", 5}          /* JSON_ATOM_FIELD */
};
static Tcl_Obj *NsJsonAtomObjs[JSON_ATOM_MAX];

#else
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
} NsJsonAtom;

static const char *NsJsonAtomStrings[JSON_ATOM_MAX] = {
    "auto",
    "string", "number", "boolean", "null", "object", "array",
    "true", "false", "", NS_JSON_NULL_SENTINEL,
    "key", "field"
};

static Tcl_Obj *NsJsonAtomObjs[JSON_ATOM_MAX];
#endif

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
    size_t                nKeyAlloc;
    size_t                nKeyFree;
    size_t                nKeyReuse;
    size_t                nKeyObjIncr;
    size_t                nKeyObjDecr;
    size_t                nKeyAllocDropped;

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


/*
 * Local functions defined in this file
 */

static TCL_OBJCMDPROC_T JsonIsNullObjCmd;
static TCL_OBJCMDPROC_T JsonKeyDecodeObjCmd;
static TCL_OBJCMDPROC_T JsonKeyEncodeObjCmd;
static TCL_OBJCMDPROC_T JsonKeyInfoObjCmd;
static TCL_OBJCMDPROC_T JsonParseObjCmd;
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
static void          NsJsonInitAtoms(void);
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
static Tcl_HashEntry*JsonKeyTableCreate(JsonParser *jp, const char *bytes, TCL_SIZE_T len, int *isNewPtr) NS_GNUC_NONNULL(1,2,4);
static Tcl_Obj      *JsonInternKeyObj(JsonParser *jp, const char *bytes, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);

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
static void          JsonKeyPathAppendEscaped(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen) NS_GNUC_NONNULL(1,2);
static void          JsonKeyPathUnescapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen) NS_GNUC_NONNULL(1,2);

static void          JsonKeyPathAppendSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen) NS_GNUC_NONNULL(1,2);
static void          JsonKeyPathAppendIndex(Tcl_DString *dsPtr, size_t idx) NS_GNUC_NONNULL(1);
static void          JsonKeyPathMakeTypeKey(Tcl_DString *typeKeyDsPtr, const char *key, TCL_SIZE_T keyLen) NS_GNUC_NONNULL(1,2);

static void          JsonSetPutValue(Ns_Set *set, const char *key, TCL_SIZE_T keyLen, const char *val, TCL_SIZE_T valLen) NS_GNUC_NONNULL(1,2,4);
static void          JsonSetPutType(Ns_Set *set, Tcl_DString *typeKeyDsPtr, const char *key, TCL_SIZE_T keyLen, JsonValueType vt) NS_GNUC_NONNULL(1,2,3);

static void          JsonAppendQuotedString(Tcl_DString *dsPtr, const char *s, TCL_SIZE_T len) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode JsonEmitTripleAppend(Tcl_Obj *listObj, Tcl_Obj *nameObj, Tcl_Obj *typeObj, Tcl_Obj *valueObj);
static int           JsonEmitValueFromTriple(Tcl_Interp *interp, Tcl_Obj *typeObj, Tcl_Obj *valObj,
                                             bool validateNumbers, int depth, bool pretty,
                                             Tcl_DString *dsPtr) NS_GNUC_NONNULL(1,2,3,7);
static int           JsonEmitContainerFromTriples(Tcl_Interp *interp, Tcl_Obj *triplesObj, bool isObject,
                                                  bool validateNumbers, int depth, bool pretty,
                                                  Tcl_DString *dsPtr) NS_GNUC_NONNULL(1,2,7);

static Ns_ReturnCode JsonFlattenToSet(Ns_Set *set, Tcl_DString *pathDsPtr, Tcl_DString *typeKeyDsPtr,
                                      Tcl_Obj *valueObj,  const char *valStr, TCL_SIZE_T valLen, JsonValueType vt) NS_GNUC_NONNULL(1,2,3);

static int           JsonMaybeWrapScanResult(Tcl_Interp *interp, bool isScan, size_t consumed) NS_GNUC_NONNULL(1);
static int           JsonCheckTrailingDecode(Tcl_Interp *interp, const unsigned char *buf, size_t len, size_t consumed) NS_GNUC_NONNULL(1,2);


/*
 *----------------------------------------------------------------------
 *
 * DStringToObj --
 *
 *	This function moves a dynamic string's contents to a new Tcl_Obj. Be
 *	aware that this function does *not* check that the encoding of the
 *	contents of the dynamic string is correct; this is the caller's
 *	responsibility to enforce.
 *
 *      The function is essentially a copy of the internal function
 *      TclDStringToOb and is a counter part to Tcl_DStringResult.
 *
 * Results:
 *	The newly-allocated untyped (i.e., typePtr==NULL) Tcl_Obj with a
 *	reference count of zero.
 *
 * Side effects:
 *	The string is "moved" to the object. dsPtr is reinitialized to an
 *	empty string; it does not need to be Tcl_DStringFree'd after this if
 *	not used further.
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
#if SHARED_ATOMS
static void
NsJsonInitAtoms(void)
{
    NsAtomInit();
    (void) NsAtomsInit(jsonAtomSpecs, JSON_ATOM_MAX, NsJsonAtomObjs);
}
#else
static void
NsJsonInitAtoms(void)
{
    size_t i;

    for (i = 0; i < JSON_ATOM_MAX; i++) {
        NsJsonAtomObjs[i] = Tcl_NewStringObj(NsJsonAtomStrings[i], TCL_INDEX_NONE);
        Tcl_IncrRefCount(NsJsonAtomObjs[i]);
    }

    /*
     * Optional: boolean atoms as actual boolean objects (faster in dict/list)
     * Keep separate if you want both spellings and typed bools.
     */
    /* NsJsonTrueObj = Tcl_NewBooleanObj(1); Tcl_IncrRefCount(...); etc. */
}
#endif

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
    memcpy(dst, bytes, len);
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
static unsigned int
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
 * JsonKeyTableCreate --
 *
 *      Create (or intern) a key in the JSON key sharing table.
 *
 * Results:
 *      Pointer to the canonical, stable, NUL-terminated key buffer for the
 *      interned key.
 *
 * Side effects:
 *      May allocate memory for a new key and insert a new entry into the
 *      hash table.
 *
 *----------------------------------------------------------------------
 */
static inline Tcl_HashEntry *
JsonKeyTableCreate(JsonParser *jp, const char *bytes, TCL_SIZE_T len, int *isNewPtr)
{
    JsonKey probe;
    probe.bytes = bytes;
    probe.len   = len;

    return Tcl_CreateHashEntry(&jp->keyTable, (const char *)&probe, isNewPtr);
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

    hPtr = JsonKeyTableCreate(jp, bytes, len, &isNew);

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
        *valueObjPtr = NsJsonAtomObjs[JSON_ATOM_TRUE];
        *valueTypePtr = JSON_VT_BOOL;
        return NS_OK;
    }
    if (jp->end - p >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') {
        jp->p += 5;
        *valueObjPtr = NsJsonAtomObjs[JSON_ATOM_FALSE];
        *valueTypePtr = JSON_VT_BOOL;
        return NS_OK;
    }
    if (jp->end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') {
        jp->p += 4;
        *valueObjPtr = NsJsonAtomObjs[JSON_ATOM_VALUE_NULL];
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
#include <math.h>

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
        if (JsonFlattenToSet(set, pathDsPtr, typeKeyDsPtr, valObj, s, len, *valueTypePtr) != NS_OK) {
            Ns_DStringPrintf(jp->errDsPtr, "ns_json: internal error flattening string");
            goto done;
        }
        rc = NS_OK;
        goto done;

    case 't':
    case 'f':
    case 'n':
        if (JsonParseLiteral(jp, &valObj, valueTypePtr) != NS_OK) {
            goto done;
        }
        Tcl_IncrRefCount(valObj);
        if (JsonFlattenToSet(set, pathDsPtr, typeKeyDsPtr, valObj, NULL, 0, *valueTypePtr) != NS_OK) {
            Ns_DStringPrintf(jp->errDsPtr, "ns_json: internal error flattening literal");
            goto done;
        }
        rc = NS_OK;
        goto done;

    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (JsonParseNumber(jp, &valObj, valueTypePtr) != NS_OK) {
                goto done;
            }
            Tcl_IncrRefCount(valObj);
            if (JsonFlattenToSet(set, pathDsPtr, typeKeyDsPtr, valObj, NULL, 0, *valueTypePtr) != NS_OK) {
                Ns_DStringPrintf(jp->errDsPtr, "ns_json: internal error flattening number");
                goto done;
            }
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
            if (JsonEmitTripleAppend(accObj,
                                     keyObj,
                                     NsJsonAtomObjs[*valueTypePtr],
                                     valObj) != NS_OK) {
                jp->depth--;
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: internal error building triples");
                return NS_ERROR;
            }
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

    if (jp->opt->output == NS_JSON_OUTPUT_TRIPLES) {
        accObj = Tcl_NewListObj(0, NULL);
    } else {
        accObj = Tcl_NewListObj(0, NULL);
    }

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

            if (JsonEmitTripleAppend(accObj,
                                     nameObj,
                                     NsJsonAtomObjs[*valueTypePtr],
                                     valObj) != NS_OK) {
                jp->depth--;
                Ns_DStringPrintf(jp->errDsPtr,
                                 "ns_json: internal error building triples");
                return NS_ERROR;
            }
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
 *      This helper is intended for JSON generation paths (e.g.,
 *      ns_json value -validatenumbers), where a Tcl value is emitted as a
 *      JSON number literal. Validation is purely lexical and does not perform
 *      numeric conversion.
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
 * JsonKeyPathAppendEscaped --
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
JsonKeyPathAppendEscaped(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen)
{
    const char *p = seg;
    const char *end = seg + segLen;

    while (p < end) {
        const char c = *p++;
        switch (c) {
        case '~':
            Tcl_DStringAppend(dsPtr, "~0", 2);
            break;
        case '/':
            Tcl_DStringAppend(dsPtr, "~1", 2);
            break;
        case '.':
            Tcl_DStringAppend(dsPtr, "~2", 2);
            break;
        default:
            Tcl_DStringAppend(dsPtr, &c, 1);
            break;
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
 *      JsonKeyPathAppendEscaped().
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
JsonKeyPathUnescapeSegment(Tcl_DString *dsPtr, const char *seg, TCL_SIZE_T segLen)
{
    const char *p = seg;
    const char *end = seg + segLen;

    while (p < end) {
        char c = *p++;
        if (c == '~' && p < end) {
            char e = *p++;
            switch (e) {
            case '0': Tcl_DStringAppend(dsPtr, "~", 1); break;
            case '1': Tcl_DStringAppend(dsPtr, "/", 1); break;
            case '2': Tcl_DStringAppend(dsPtr, ".", 1); break;
            default:
                Tcl_DStringAppend(dsPtr, "~", 1);
                Tcl_DStringAppend(dsPtr, &e, 1);
                break;
            }
        } else {
            Tcl_DStringAppend(dsPtr, &c, 1);
        }
    }
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
    JsonKeyPathAppendEscaped(dsPtr, seg, segLen);
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
    int  n = snprintf(buf, sizeof(buf), "%lu", (unsigned long)idx);

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
    typeString = Tcl_GetStringFromObj(NsJsonAtomObjs[vt], &len);
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
 *      This helper is used to build the triples representation for JSON
 *      containers.
 *
 * Results:
 *      NS_OK on success, NS_ERROR if list append operations fail.
 *
 * Side Effects:
 *      Extends listObj by three elements.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonEmitTripleAppend(Tcl_Obj *listObj,
                     Tcl_Obj *nameObj,
                     Tcl_Obj *typeObj,
                     Tcl_Obj *valueObj)
{
    (void) Tcl_ListObjAppendElement(NULL, listObj, nameObj);
    (void) Tcl_ListObjAppendElement(NULL, listObj, typeObj);
    (void) Tcl_ListObjAppendElement(NULL, listObj, valueObj);
    return NS_OK;
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
 *      representation.  The function uses the current path stored in pathDsPtr
 *      and maintains the corresponding type sidecar key in typeKeyDsPtr.  The
 *      JSON type is provided in typeStr and determines how the value is stored
 *      (e.g., scalar value entry, null representation, or container handling).
 *
 * Results:
 *      NS_OK on success, NS_ERROR on internal errors with a message appended
 *      to jp->errDsPtr.
 *
 * Side Effects:
 *      Adds entries to the Ns_Set and may modify pathDsPtr/typeKeyDsPtr.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
JsonFlattenToSet(Ns_Set *set,
                 Tcl_DString *pathDsPtr,
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
    if (keyLen == 0) {
        return NS_OK;
    }

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

    return NS_OK;
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

    NS_INIT_ONCE(NsJsonInitAtoms);

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
                lv[0] = NsJsonAtomObjs[JSON_ATOM_EMPTY];
                lv[1] = NsJsonAtomObjs[vt];
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
        TCL_SIZE_T  n = 0;
        const char *s = Tcl_GetStringFromObj(valueObj, &n);

        if ((size_t)n == NS_JSON_NULL_SENTINEL_LEN
            && memcmp(s, NS_JSON_NULL_SENTINEL, NS_JSON_NULL_SENTINEL_LEN) == 0) {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(1));
        } else {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
        }
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
    JsonKeyPathUnescapeSegment(&ds, s, len);
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
    JsonKeyPathAppendEscaped(&ds, s, len);
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
                JsonKeyPathUnescapeSegment(&segDs, segStart, segLen);

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
                   NsJsonAtomObjs[JSON_ATOM_KEY],
                   DStringToObj(&outDs));

    Tcl_DictObjPut(interp, dictObj,
                   NsJsonAtomObjs[JSON_ATOM_FIELD],
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
    int           validateNumbers = 0, pretty = 0;
    Ns_ObjvSpec   opts[] = {
        {"-type",            Ns_ObjvIndex, &vt, jsonValueTypes},
        {"-pretty",          Ns_ObjvBool,  &pretty,          INT2PTR(NS_TRUE)},
        {"-validatenumbers", Ns_ObjvBool,  &validateNumbers, INT2PTR(NS_TRUE)},
        {"--",               Ns_ObjvBreak, NULL, NULL},
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
            /*
             * Conservative AUTO: try int/double/boolean, else string
             */
            int         b;
            TCL_SIZE_T len;
            const char *s = Tcl_GetStringFromObj(valueObj, &len);

            if (JsonNumberLexemeIsValid((const unsigned char *)s, (size_t)len)) {
                vt = JSON_VT_NUMBER;
            } else if (! (s[0] == '0' && len > 1) && Tcl_GetBooleanFromObj(NULL, valueObj, &b) == TCL_OK) {
                vt = JSON_VT_BOOL;
            } else if ((size_t)len == NS_JSON_NULL_SENTINEL_LEN
                       && memcmp(s, NS_JSON_NULL_SENTINEL, NS_JSON_NULL_SENTINEL_LEN) == 0) {
                vt = JSON_VT_NULL;
            } else {
                vt = JSON_VT_STRING;
            }
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
                Tcl_DStringFree(&ds);
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
            if (JsonEmitContainerFromTriples(interp, valueObj, (vt == JSON_VT_OBJECT),
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
        {"value",      JsonValueObjCmd},
        {NULL, NULL}
    };
    NS_INIT_ONCE(NsJsonInitAtoms);

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
