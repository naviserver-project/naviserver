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
 * set.c --
 *
 *      Implements the Ns_Set data type.
 */

#include "nsd.h"

/*
 * Typedefs of functions
 */
typedef int (*StringCmpProc)(const char *s1, const char *s2);
typedef int (*SetFindProc)(const Ns_Set *set, const char *key);

/*
 * Local functions defined in this file
 */

static void SetMerge(Ns_Set *high, const Ns_Set *low, SetFindProc findProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void SetCopyElements(const char*msg, const Ns_Set *from, Ns_Set *const to)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static const char *SetGetValueCmp(const Ns_Set *set, const char *key, const char *def, StringCmpProc cmp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static Ns_Set *SetCreate(const char *name, size_t size);

#ifdef NS_SET_DSTRING
static void ShiftData(Ns_Set *set, const char *oldDataStart)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static char *AppendData(Ns_Set *set, size_t index, const char *value, TCL_SIZE_T valueSize)
    NS_GNUC_NONNULL(1);
#endif

#if defined(NS_SET_DEBUG)
static void hexPrint(const char *msg, const unsigned char *octets, size_t octetLength)
{
    if (Ns_LogSeverityEnabled(Notice)) {
        size_t i;
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        Ns_DStringPrintf(&ds, "%s (len %" PRIuz "): ", msg, octetLength);
        for (i = 0; i < octetLength; i++) {
            Ns_DStringPrintf(&ds, "%.2x ", octets[i] & 0xff);
        }
        Ns_Log(Notice, "%s", ds.string);
        Tcl_DStringInit(&ds);

        Ns_DStringPrintf(&ds, "%s (len %" PRIuz "): ", msg, octetLength);
        for (i = 0; i < octetLength; i++) {
            if (octets[i] < 20) {
                Ns_DStringPrintf(&ds, "%-2c ", 32);
            } else {
                Ns_DStringPrintf(&ds, "%-2c ", octets[i]);
            }
        }
        Ns_Log(Notice, "%s", ds.string);
        Tcl_DStringFree(&ds);
    }
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIUpdate --
 *
 *      Update tuple or add it (case insensitive)
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_SetIUpdateSz(Ns_Set *set,
                const char *keyString, TCL_SIZE_T keyLength,
                const char *valueString, TCL_SIZE_T valueLength)
{
    ssize_t index;
    size_t result;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);

    index = Ns_SetIFind(set, keyString);
    if (index != -1) {
        Ns_SetPutValueSz(set, (size_t)index, valueString, valueLength);
        /*
         * If the capitalization of the key is different, keep the new one.
         */
        if (*(set->fields[index].name) != *keyString) {
            if (keyLength == TCL_INDEX_NONE) {
                keyLength = (TCL_SIZE_T)strlen(keyString);
            }
            memcpy(set->fields[index].name, keyString, (size_t)keyLength);
        }
        result = (size_t)index;
    } else {
        result = Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
    }
    return result;
}

size_t
Ns_SetIUpdate(Ns_Set *set, const char *keyString, const char *valueString)
{
    return Ns_SetIUpdateSz(set, keyString, TCL_INDEX_NONE, valueString, TCL_INDEX_NONE);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetUpdate --
 *
 *      Update tuple or add it (case sensitive)
 *
 * Results:
 *      Index value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
size_t
Ns_SetUpdateSz(Ns_Set *set,
               const char *keyString, TCL_SIZE_T keyLength,
               const char *valueString, TCL_SIZE_T valueLength)
{
    ssize_t index;
    size_t result;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);

    index = Ns_SetFind(set, keyString);
    if (index != -1) {
        Ns_SetPutValueSz(set, (size_t)index, valueString, valueLength);
        result = (size_t)index;
    } else {
        result = Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
    }
    Ns_Log(Ns_LogNsSetDebug, "Ns_SetUpdateSz %p '%s': index %ld key '%s' value '%s'",
           (void*)set, set->name, index, keyString, valueString);
    return result;
}

size_t
Ns_SetUpdate(Ns_Set *set, const char *keyString, const char *valueString)
{
    return Ns_SetUpdateSz(set, keyString, TCL_INDEX_NONE, valueString, TCL_INDEX_NONE);
}

#ifdef NS_SET_DSTRING
/*
 *----------------------------------------------------------------------
 *
 * ShiftData --
 *
 *      When the string buffer of an Ns_Set has shifted (e.g., due to realloc
 *      from static to dynamic memory) the addresses of the keys and values of
 *      the ns_set are readjusted by this function
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
ShiftData(Ns_Set *set, const char *oldDataStart) {
    const char *newDataStart = set->data.string;

    if (oldDataStart != newDataStart && set->size > 0) {
        ptrdiff_t shift = newDataStart - oldDataStart;
        size_t    i;

        Ns_Log(Ns_LogNsSetDebug, "... shift %lu elements", set->size);

        for (i = 0u; i < set->size; i++) {
            set->fields[i].name += shift;
            if (set->fields[i].value != NULL) {
                set->fields[i].value += shift;
            }
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * AppendData --
 *
 *      Add string with a given size to the Tcl_DString set->data, "value"
 *      might be NULL. When the underlying set->data.string is reallocated,
 *      all the existing "name" and "value" fields are adjusted.
 *
 * Results:
 *      Start of the inserted string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static char *
AppendData(Ns_Set *set, size_t index, const char *value, TCL_SIZE_T valueSize)
{
    char      *oldDataStart;
    TCL_SIZE_T oldLength, oldAvail;

    oldDataStart = set->data.string;
    oldLength = set->data.length;
    oldAvail = set->data.spaceAvl;
    if (valueSize == TCL_INDEX_NONE && value == NULL) {
        valueSize = 0;
    }
    Tcl_DStringAppend(&set->data, value, (TCL_SIZE_T)valueSize);
    if (value != NULL) {
        Tcl_DStringSetLength(&set->data, set->data.length + 1);
        set->data.string[set->data.length-1] = '\0';
    }
    if (oldDataStart != set->data.string) {
        Ns_Log(Ns_LogNsSetDebug, "MUST SHIFT %p '%s':"
               " length %" PRITcl_Size "->%" PRITcl_Size
               " buffer %" PRITcl_Size "->%" PRITcl_Size
               " (while appending %ld '%s')",
               (void*)set, set->name,
               oldLength, set->data.length,
               oldAvail, set->data.spaceAvl,
               index, value);
        ShiftData(set, oldDataStart);
    }

    return likely(value != NULL) ? set->data.string + oldLength : NULL;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * NsSetResize --
 *
 *      Function for internal usage to resize a set. If the number of elements
 *      is increased or decreased, the fields are reallocated. A decrease
 *      might entail an Ns_SetTrunc operation. Similarly, the string buffer
 *      can be increased or decreased by this function.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potentially reallocation of the involved memory regions.
 *
 *----------------------------------------------------------------------
 */
void
NsSetResize(Ns_Set *set, size_t newSize, int bufferSize)
{
#ifdef NS_SET_DSTRING
    const char *oldDataStart;
#else
    (void)bufferSize;
#endif
    if (newSize != set->size) {
        if (newSize < set->size) {
            Ns_SetTrunc(set, newSize);
        }
        set->maxSize = newSize+1;
        set->fields = ns_realloc(set->fields,
                                 sizeof(Ns_SetField) * set->maxSize);
    }
#ifdef NS_SET_DSTRING
    oldDataStart = set->data.string;
    Ns_SetDataPrealloc(set, (TCL_SIZE_T)bufferSize);
    ShiftData(set, oldDataStart);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetCreate --
 *
 *      Initialize a new set.
 *
 * Results:
 *      A pointer to a new set.
 *
 * Side effects:
 *      Memory is allocated; free with Ns_SetFree.
 *
 *----------------------------------------------------------------------
 */
static long createdSets = 0;
static Ns_Set *
SetCreate(const char *name, size_t size)
{
    Ns_Set *setPtr;

    createdSets++;
    setPtr = ns_malloc(sizeof(Ns_Set));
    setPtr->size = 0u;
    setPtr->maxSize = size;
    setPtr->name = ns_strcopy(name);
    setPtr->fields = ns_calloc(1u, sizeof(Ns_SetField) * setPtr->maxSize);
#ifdef NS_SET_DSTRING
    Tcl_DStringInit(&setPtr->data);
#endif

#ifdef NS_SET_DEBUG
    Ns_Log(Notice, "SetCreate %p '%s': size %ld/%ld (created %ld)",
           (void*)setPtr, setPtr->name, size, setPtr->maxSize, createdSets);
#endif

    return setPtr;
}

Ns_Set *
Ns_SetCreate(const char *name)
{
    return SetCreate(name, 10u);
}

Ns_Set *
Ns_SetCreateSz(const char *name, size_t size)
{
    return SetCreate(name, size);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetFree --
 *
 *      Free a set and its associated data with ns_free.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free both the Ns_Set structure AND its tuples.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetFree(Ns_Set *set)
{
    if (set != NULL) {
#ifndef NS_SET_DSTRING
        size_t i;
#endif
        assert(set->size <  set->maxSize);
        createdSets --;

#ifdef NS_SET_DSTRING
        Ns_Log(Ns_LogNsSetDebug,
               "Ns_SetFree %p '%s': size %ld/%ld"
               " data %" PRITcl_Size "/%" PRITcl_Size
               " (created %ld)",
                (void*)set, set->name, set->size, set->maxSize,
                set->data.length, set->data.spaceAvl, createdSets);
        Tcl_DStringFree(&set->data);
#else
        Ns_Log(Ns_LogNsSetDebug, "Ns_SetFree %p '%s': elements %ld",
               (void*)set, set->name, set->size);

        for (i = 0u; i < set->size; ++i) {
            Ns_Log(Ns_LogNsSetDebug, "... %ld: key <%s> value <%s>",
                   i, set->fields[i].name, set->fields[i].value);

            ns_free(set->fields[i].name);
            ns_free(set->fields[i].value);
        }
#endif
        ns_free(set->fields);
        ns_free((char *)set->name);
        ns_free(set);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetPut --
 *
 *      Insert (add) a tuple into an existing set.
 *
 * Results:
 *      The index number of the new tuple.
 *
 * Side effects:
 *      The key/value will be strdup'ed.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_SetPutSz(Ns_Set *set,
            const char *keyString, TCL_SIZE_T keyLength,
            const char *valueString, TCL_SIZE_T valueLength)
{
    size_t idx;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);

    assert(set->size <=  set->maxSize);
    idx = set->size;
    set->size++;

    if (set->size >= set->maxSize) {
        size_t oldSize = set->size;

        set->maxSize = set->size * 2u;
        set->fields = ns_realloc(set->fields,
                                 sizeof(Ns_SetField) * set->maxSize);
        Ns_Log(Ns_LogNsSetDebug, "Ns_SetPutSz %p '%s': [%lu] realloc from %lu to maxsize %lu"
               " (while adding '%s')",
               (void*)set, set->name, idx, oldSize, set->maxSize, valueString);
        memset(&set->fields[idx], 0, sizeof(Ns_SetField) * (set->maxSize - set->size));
    }
#ifdef NS_SET_DSTRING
    set->fields[idx].name = AppendData(set, idx, keyString, keyLength);
    set->fields[idx].value = AppendData(set, idx, valueString, valueLength);
#else
    set->fields[idx].name = ns_strncopy(keyString, keyLength);
    set->fields[idx].value = ns_strncopy(valueString, valueLength);
#endif
    Ns_Log(Ns_LogNsSetDebug, "Ns_SetPut %p [%lu] key '%s' value '%s' size %" PRITcl_Size,
           (void*)set, idx, set->fields[idx].name, set->fields[idx].value, valueLength);
    return idx;
}

size_t
Ns_SetPut(Ns_Set *set, const char *key, const char *value)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetPutSz(set, key, TCL_INDEX_NONE, value, TCL_INDEX_NONE);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetUniqueCmp --
 *
 *      Using the comparison function, see if multiple keys match
 *      key.
 *
 * Results:
 *      NS_FALSE: multiple keys match key
 *      NS_TRUE: 0 or 1 keys match key.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetUniqueCmp(const Ns_Set *set, const char *key, StringCmpProc cmp)
{
    size_t i;
    bool   found, result = NS_TRUE;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);

    found = NS_FALSE;
    for (i = 0u; i < set->size; ++i) {
        const char *name = set->fields[i].name;

        if ((name == NULL) || (((*cmp) (key, name)) == 0)) {

            if (found) {
                result = NS_FALSE;
                break;
            }
            found = NS_TRUE;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetFindCmp --
 *
 *      Returns the index of a tuple matching key, using a comparison
 *      function callback.
 *
 * Results:
 *      A tuple index or -1 if no matches.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetFindCmp(const Ns_Set *set, const char *key, StringCmpProc cmp)
{
    size_t i;
    int    result = -1;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);

    for (i = 0u; i < set->size; i++) {
        const char *name = set->fields[i].name;

        assert(name != NULL);
        if (((*cmp) (key, name)) == 0) {
            result = (int)i;
            break;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetGetCmp --
 *
 *      Returns the value of a tuple matching key, using a comparison
 *      function callback.
 *
 * Results:
 *      A value or NULL if no matches.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SetGetCmp(const Ns_Set *set, const char *key, StringCmpProc cmp)
{
    int idx;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);

    idx = Ns_SetFindCmp(set, key, cmp);
    return ((idx == -1) ? NULL : set->fields[idx].value);
}


/*
 *----------------------------------------------------------------------
 *
 * NsSetGetCmpDListAppend --
 *
 *      Retrieve one or all values for a key from an Ns_Set. The function
 *      returns the number of elements retrieved depending on the Boolean
 *      argument "all". The resulting list contains pointers to the actual
 *      string values in the Ns_Set structure. The strings are volatile and
 *      should be copied immediately in case the Ns_Set is modified in the
 *      same call.
 *
 * Results:
 *      Number of matching keys in the set.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

size_t
NsSetGetCmpDListAppend(const Ns_Set *set, const char *key, bool all, StringCmpProc cmp, Ns_DList *dlPtr)
{
    size_t idx, count = 0u;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);
    NS_NONNULL_ASSERT(dlPtr != NULL);

    for (idx = 0u; idx < set->size; idx++) {
        const char *name = set->fields[idx].name;

        assert(name != NULL);
        if (((*cmp) (key, name)) == 0) {
            count ++;
            Ns_DListAppend(dlPtr, set->fields[idx].value);
            if (!all) {
                break;
            }
        }
    }
    return count;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetUnique --
 *
 *      Check if a key in a set is unique (case sensitive).
 *
 * Results:
 *      NS_TRUE if unique, NS_FALSE if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetUnique(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetUniqueCmp(set, key, strcmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIUnique --
 *
 *      Check if a key in a set is unique (case insensitive).
 *
 * Results:
 *      NS_TRUE if unique, NS_FALSE if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetIUnique(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetUniqueCmp(set, key, strcasecmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetFind --
 *
 *      Locate the index of a field in a set (case sensitive)
 *
 * Results:
 *      A field index or -1 if not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetFind(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetFindCmp(set, key, strcmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIFind --
 *
 *      Locate the index of a field in a set (case insensitive)
 *
 * Results:
 *      A field index or -1 if not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetIFind(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetFindCmp(set, key, strcasecmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetGet --
 *
 *      Return the value associated with a key, case sensitive.
 *
 * Results:
 *      A value or NULL if key not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SetGet(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetGetCmp(set, key, strcmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIGet --
 *
 *      Return the value associated with a key, case insensitive.
 *
 * Results:
 *      A value or NULL if key not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SetIGet(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetGetCmp(set, key, strcasecmp);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetGetValue, Ns_SetIGetValue --
 *
 *      Return the value associated with a key. The variant SetIGetValue is
 *      not case sensitive.  If no value found or it is empty, return provided
 *      default value
 *
 * Results:
 *      A value or NULL if key not found or default is NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static const char *
SetGetValueCmp(const Ns_Set *set, const char *key, const char *def, StringCmpProc cmp)
{
    const char *value;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);

    value = Ns_SetGetCmp(set, key, cmp);

    if (value == NULL || *value == '\0') {
        value = def;
    }
    return value;
}

const char *
Ns_SetGetValue(const Ns_Set *set, const char *key, const char *def)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return SetGetValueCmp(set, key, def, strcmp);
}

const char *
Ns_SetIGetValue(const Ns_Set *set, const char *key, const char *def)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return SetGetValueCmp(set, key, def, strcasecmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetTrunc --
 *
 *      Remove all tuples after 'size'
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free tuple memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetTrunc(Ns_Set *set, size_t size)
{
    NS_NONNULL_ASSERT(set != NULL);

# ifdef NS_SET_DEBUG
    Ns_Log(Notice, "Ns_SetTrunc %p '%s' to %lu data avail %d",
           (void*)set, set->name, size, set->data.spaceAvl);
# endif

    if (size < set->size) {
#ifdef NS_SET_DSTRING
        if (size == 0u) {
            Tcl_DStringSetLength(&set->data, 0);
        } else {
            size_t i;
            const char *endPtr = set->fields[size].name;

# ifdef NS_SET_DEBUG
            hexPrint("before trunc", (unsigned char *)set->data.string, (size_t)set->data.length);
            Ns_SetPrint(set);
# endif
            //Ns_Log(Notice, "... initial endPtr %p len %ld", (void*)endPtr, endPtr-set->data.string);
            for (i = 0; i <= size; i++) {
                if (set->fields[i].name > endPtr) {
                    endPtr = set->fields[i].name + strlen(set->fields[i].name) + 1;
                    //Ns_Log(Notice, "... ext1 endPtr %p len %ld", (void*)endPtr, endPtr-set->data.string);
                }
                if (set->fields[i].value > endPtr) {
                    endPtr = set->fields[i].value + strlen(set->fields[i].value) + 1;
                    //Ns_Log(Notice, "... [%lu] ext2 endPtr %p len %ld", i, (void*)endPtr, endPtr-set->data.string);
                }
            }
            //Ns_Log(Notice, "... final can trunc data from %i to %ld",  set->data.length, endPtr-set->data.string);
            Tcl_DStringSetLength(&set->data, (TCL_SIZE_T)(endPtr - set->data.string));

# ifdef NS_SET_DEBUG
            hexPrint("after trunc", (unsigned char *)set->data.string, (size_t)set->data.length);
            Ns_SetPrint(set);
# endif

        }
        /*
         * We could consider shrinking extensively large data blocks via
         * realloc();
         * https://stackoverflow.com/questions/7078019/using-realloc-to-shrink-the-allocated-memory
         */
#else
        size_t idx;
        for (idx = size; idx < set->size; idx++) {
            ns_free(set->fields[idx].name);
            ns_free(set->fields[idx].value);
        }
#endif
        set->size = size;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetDelete --
 *
 *      Delete a tuple from a set. If index is -1, this function does
 *      nothing.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free tuple memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetDelete(Ns_Set *set, int index)
{
    NS_NONNULL_ASSERT(set != NULL);

    if ((index != -1) && (index < (int)set->size)) {
        size_t i;

#ifdef NS_SET_DSTRING
        Ns_Log(Ns_LogNsSetDebug, "Ns_SetDelete %p '%s': on %d %s: '%s'",
               (void*)set, set->name, index,
               set->fields[index].name,
               set->fields[index].value);
        /*
         * The string values for "name" and "value" are still kept in the
         * Tcl_DString.
         */
#else
        ns_free(set->fields[index].name);
        ns_free(set->fields[index].value);
#endif
        --set->size;
        for (i = (size_t)index; i < set->size; ++i) {
            set->fields[i].name = set->fields[i + 1u].name;
            set->fields[i].value = set->fields[i + 1u].value;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetPutValue --
 *
 *      Set the value for a given tuple.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free the old value dup the new value.
 *
 *----------------------------------------------------------------------
 */
void
Ns_SetPutValue(Ns_Set *set, size_t index, const char *value)
{
    Ns_SetPutValueSz(set, index, value, TCL_INDEX_NONE);
}

void
Ns_SetPutValueSz(Ns_Set *set, size_t index, const char *value, TCL_SIZE_T size)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    Ns_Log(Ns_LogNsSetDebug, "Ns_SetPutValue %p [%lu] key '%s' value '%s' size %" PRITcl_Size,
           (void*)set, index, set->fields[index].name, value, size);

    if (index < set->size) {
#ifdef NS_SET_DSTRING
        if (size == TCL_INDEX_NONE) {
            size = (TCL_SIZE_T)strlen(value);
        }
#ifdef NS_SET_DEBUG
        Ns_Log(Notice, "Ns_SetPutValue %p [%lu] key '%s' value '%s' size %ld",
               (void*)set, index, set->fields[index].name, value, size);
#endif

        if (set->size > 0) {
            size_t  oldSize = 0u;

            oldSize = set->fields[index].value != NULL ? strlen(set->fields[index].value) : 0;

            if (oldSize == 0 && size == 0) {
                /*
                 * Nothing to do.
                 */
            } else if (set->fields[index].value == value && oldSize == (size_t)size) {
                /*
                 * Old value is the same as the new value (same address, same size)
                 */
                Ns_Log(Debug, "Ns_SetPutValueSz %p: old value is the same as the new value: '%s'",
                       (void*)set, value);
            } else if (oldSize >= (size_t)size && oldSize != 0) {
                /*
                 * New value fits old slot
                 */
                memcpy(set->fields[index].value, value, (size_t)size);
                set->fields[index].value[size] = '\0';
            } else {
                /*
                 * Invalidate old value and append new value.
                 */
                if (set->fields[index].value != NULL) {
                    *(set->fields[index].value) = '\3';
                }
                set->fields[index].value = AppendData(set, index, value, size);
            }
        } else {
            Tcl_Panic("Ns_SetPutValueSz called on a set with size 0");
        }
#else
        if (set->fields[index].value != value) {
            ns_free(set->fields[index].value);
            set->fields[index].value = ns_strncopy(value, size);
        } else {
            Ns_Log(Debug, "Ns_SetPutValueSz %p: old value is the same as the new value: '%s'",
                   (void*)set, value);
        }
#endif
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetClearValues --
 *
 *      Clear all values in the specified Ns_Set.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will make space available in the Tcl_DString
 *      (or ns_free the values).
 *
 *----------------------------------------------------------------------
 */
void Ns_SetClearValues(Ns_Set *set, TCL_SIZE_T maxAlloc)
{
    size_t i;

#ifdef NS_SET_DSTRING
    bool mustShift = NS_FALSE;

    for (i = 0u; i < set->size; ++i) {
        if (set->fields[i].value != NULL) {
            mustShift = NS_TRUE;
            break;
        }
    }
    Ns_Log(Ns_LogNsSetDebug,
           "Ns_SetClearValues %p '%s': size %ld/%ld"
           " data %" PRITcl_Size "/%" PRITcl_Size
           " (created %ld)",
           (void*)set, set->name, set->size, set->maxSize,
           set->data.length, set->data.spaceAvl, createdSets);

    if (mustShift) {
        Tcl_DString ds, *dsPtr = &ds;
        Ns_DList    dl, *dlPtr = &dl;
        char       *p;
        TCL_SIZE_T  oldLength = set->data.length;

        Tcl_DStringInit(dsPtr);
        Ns_DListInit(dlPtr);
        for (i = 0u; i < set->size; ++i) {
            size_t nameSize = strlen(set->fields[i].name);

            Tcl_DStringAppend(dsPtr, set->fields[i].name, (TCL_SIZE_T)nameSize);
            Tcl_DStringSetLength(dsPtr, dsPtr->length+1);
            Ns_DListAppend(dlPtr, (void*)(ptrdiff_t)(nameSize+1));
            set->fields[i].value = NULL;
        }
        Tcl_DStringSetLength(&set->data, dsPtr->length);

        /*
         * In cases, where the allocated memory was larger than maxAlloc, and
         * the actually needed amount is less than a quarter, shrink the
         * buffer. We do not have to use realloc(), since the content is
         * anyhow copied later. Note that we have to use the same
         * alloc()/free() functions that also Tcl uses.
         */
        if (set->data.spaceAvl > maxAlloc && (oldLength < maxAlloc/4)) {
            const char *oldBuffer = set->data.string;

            set->data.string = ckalloc((size_t)maxAlloc);
            ckfree((void*)oldBuffer);
            set->data.spaceAvl = maxAlloc;
        }
        memcpy(set->data.string, dsPtr->string, (size_t)dsPtr->length);

        p = set->data.string;
        set->fields[0].name = p;
        for (i = 1u; i < set->size; ++i) {
            p += (ptrdiff_t)(dlPtr->data[i-1]);
            set->fields[i].name = p;
        }
        Tcl_DStringFree(dsPtr);
        Ns_DListFree(dlPtr);

        Ns_Log(Ns_LogNsSetDebug,
           "... final size %ld/%ld data %" PRITcl_Size "/%" PRITcl_Size,
           set->size, set->maxSize,
           set->data.length, set->data.spaceAvl);
    }

#else
    (void)maxAlloc;
    for (i = 0u; i < set->size; ++i) {
        ns_free(set->fields[i].value);
        set->fields[i].value = NULL;
    }
#endif
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_SetDeleteKey --
 *
 *      Delete a tuple from the set (case sensitive).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free tuple memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetDeleteKey(Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    Ns_SetDelete(set, Ns_SetFind(set, key));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIDeleteKey --
 *
 *      Delete a tuple from the set (case insensitive).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free tuple memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetIDeleteKey(Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    Ns_SetDelete(set, Ns_SetIFind(set, key));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetListFind --
 *
 *      In a null-terminated array of sets, find the set with the
 *      given name.
 *
 * Results:
 *      A set, or NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_SetListFind(Ns_Set *const *sets, const char *name)
{
    Ns_Set *result = NULL;

    NS_NONNULL_ASSERT(sets != NULL);

    while (*sets != NULL) {
        if (name == NULL) {
            if ((*sets)->name == NULL) {
                result = *sets;
                break;
            }
        } else {
            if ((*sets)->name != NULL &&
                STREQ((*sets)->name, name)) {
                result = *sets;
                break;
            }
        }
        ++sets;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetSplit --
 *
 *      Split a set into an array of new sets. This assumes that each
 *      key name in the fields of a set contains a separating
 *      character. The fields of the set are partitioned into new
 *      sets whose set names are the characters before the separator
 *      and whose field key names are the characters after the
 *      separator.
 *
 * Results:
 *      A new set.
 *
 * Side effects:
 *      Will allocate a new set and tuples.
 *
 *----------------------------------------------------------------------
 */

Ns_Set **
Ns_SetSplit(const Ns_Set *set, char sep)
{
    size_t        i;
    Ns_DString    ds;
    const Ns_Set *end = NULL;

    NS_NONNULL_ASSERT(set != NULL);

    Ns_DStringInit(&ds);
    Ns_DStringNAppend(&ds, (char *) &end, (TCL_SIZE_T)sizeof(Ns_Set *));

    for (i = 0u; i < set->size; ++i) {
        Ns_Set     *next;
        const char *name;
        char       *key;

        key = strchr(set->fields[i].name, (int)sep);
        if (key != NULL) {
            *key++ = '\0';
            name = set->fields[i].name;
        } else {
            key = set->fields[i].name;
            name = NULL;
        }
        next = Ns_SetListFind((Ns_Set **) ds.string, name);
        if (next == NULL) {
            Ns_Set        **sp;

            next = Ns_SetCreate(name);
            sp = (Ns_Set **) (ds.string + ds.length - sizeof(Ns_Set *));
            *sp = next;
            Ns_DStringNAppend(&ds, (char *) &end, (TCL_SIZE_T)sizeof(Ns_Set *));
        }
        (void)Ns_SetPut(next, key, set->fields[i].value);
        if (name != NULL) {
            *(key-1) = sep;
        }
    }
    return (Ns_Set **) Ns_DStringExport(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DStringAppendSet --
 *
 *      Add the content (not including the name) to the
 *      provided Ns_DString, which has to be initialized.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Update Ns_DString.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DStringAppendSet(Ns_DString *dsPtr, const Ns_Set *set)
{
    size_t i;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    for (i = 0u; i < Ns_SetSize(set); ++i) {
        Tcl_DStringAppendElement(dsPtr, Ns_SetKey(set, i));
        Tcl_DStringAppendElement(dsPtr, Ns_SetValue(set, i));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetListFree --
 *
 *      Free a null-terminated array of sets.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free all sets in the array and their tuples.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetListFree(Ns_Set **sets)
{
   Ns_Set *const*s;

    NS_NONNULL_ASSERT(sets != NULL);

    s = sets;
    while (*s != NULL) {
        Ns_SetFree(*s);
        ++s;
    }
    ns_free(sets);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetMerge, NS_SetIMerge --
 *
 *      Combine the 'low' set into the 'high' set.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will add tuples to 'high'.
 *
 *----------------------------------------------------------------------
 */
static void
SetMerge(Ns_Set *high, const Ns_Set *low, SetFindProc findProc)
{
    size_t i;

    NS_NONNULL_ASSERT(high != NULL);
    NS_NONNULL_ASSERT(low != NULL);

    for (i = 0u; i < low->size; ++i) {
        int j = (*findProc)(high, low->fields[i].name);
        if (j == -1) {
            (void)Ns_SetPut(high, low->fields[i].name, low->fields[i].value);
        }
    }
}


void
Ns_SetMerge(Ns_Set *high, const Ns_Set *low)
{
    NS_NONNULL_ASSERT(high != NULL);
    NS_NONNULL_ASSERT(low != NULL);

    SetMerge(high, low, Ns_SetFind);
}

void
Ns_SetIMerge(Ns_Set *high, const Ns_Set *low)
{
    NS_NONNULL_ASSERT(high != NULL);
    NS_NONNULL_ASSERT(low != NULL);

    SetMerge(high, low, Ns_SetIFind);
}

#ifdef NS_SET_DSTRING
/*
 *----------------------------------------------------------------------
 *
 * Ns_SetDataPrealloc --
 *
 *      Interface for sizing a (typically fresh) Ns_Set by specifying the
 *      number of allements and the Tcl_DString space.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potentially allocating memory
 *
 *----------------------------------------------------------------------
 */
void Ns_SetDataPrealloc(Ns_Set *set, TCL_SIZE_T size)
{
    TCL_SIZE_T oldStringSize = set->data.length;

    /*
     * Note that Tcl_DStringSetLength() allocates actually one byte more than
     * specified.
     */
    Tcl_DStringSetLength(&set->data, size);
    Tcl_DStringSetLength(&set->data, oldStringSize);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetCopy --
 *
 *      Make a duplicate of a set.
 *
 * Results:
 *      A new set.
 *
 * Side effects:
 *      Will copy tuples and alloc new memory for them, too.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_SetCopy(const Ns_Set *old)
{
    Ns_Set *new;

    if (old == NULL) {
        new = NULL;
    } else {
        size_t i;

        new = SetCreate(old->name, old->size + 1); /* maybe maxSize? */
#ifdef NS_SET_DSTRING
        Ns_SetDataPrealloc(new, old->data.length + 1);
#endif
        for (i = 0u; i < old->size; ++i) {
            (void)Ns_SetPut(new, old->fields[i].name, old->fields[i].value);
        }
#ifdef NS_SET_DSTRING
# ifdef NS_SET_DEBUG
        Ns_Log(Notice, "Ns_SetCopy %p '%s': done size %lu maxsize %lu data len %d avail %d",
               (void*)new, new->name,
               new->size, new->maxSize,
               new->data.length, new->data.spaceAvl);
# endif
#endif
        Ns_Log(Ns_LogNsSetDebug, "Ns_SetCopy %p '%s' to %p",
               (void*)old, old->name, (void*)new);
    }

    return new;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetMove --
 *
 *      Moves the data from one set to another, truncating the "from"
 *      set.
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
Ns_SetMove(Ns_Set *to, Ns_Set *from)
{
    size_t       i;

    NS_NONNULL_ASSERT(from != NULL);
    NS_NONNULL_ASSERT(to != NULL);

    for (i = 0u; i < from->size; i++) {
        (void)Ns_SetPut(to, from->fields[i].name, from->fields[i].value);
    }
    Ns_SetTrunc(from, 0u);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetRecreate --
 *
 *      Combination of a create and a move operation. A new set is created,
 *      all data from the old set is moved to the new set, and the old set is
 *      truncated.
 *
 * Results:
 *      new set.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Ns_Set *
Ns_SetRecreate(Ns_Set *set)
{
    Ns_Set *newSet;

    NS_NONNULL_ASSERT(set != NULL);

    newSet = ns_malloc(sizeof(Ns_Set));
    newSet->size = set->size;
    newSet->maxSize = set->maxSize;
    newSet->name = ns_strcopy(set->name);
    newSet->fields = ns_malloc(sizeof(Ns_SetField) * newSet->maxSize);
#ifdef NS_SET_DSTRING
    Tcl_DStringInit(&newSet->data);
#endif
    SetCopyElements("recreate", set, newSet);
    set->size = 0u;
#ifdef NS_SET_DSTRING
    Tcl_DStringSetLength(&set->data, 0);
#endif

    return newSet;
}

/*
 *----------------------------------------------------------------------
 *
 * SetCopyElements --
 *
 *      Copy all elements of the source to the target. The function can only
 *      be called in cases, where the caller takes care, that the target set
 *      has at least the same number of elements allocated.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SetCopyElements(const char* msg, const Ns_Set *from, Ns_Set *const to)
{
    size_t i;

#ifdef NS_SET_DSTRING
    Ns_Log(Notice, "SetCopyElements %s %p '%s': %lu elements from %p to %p",
           msg, (void*)from, from->name, from->size, (void*)from, (void*)to);

    to->size = 0u;
    for (i = 0u; i < from->size; i++) {
        Ns_SetPutSz(to,
                    from->fields[i].name, TCL_INDEX_NONE,
                    from->fields[i].value, TCL_INDEX_NONE);
    }
#else
    (void)msg;
    for (i = 0u; i < from->size; i++) {
        to->fields[i].name  = from->fields[i].name;
        to->fields[i].value = from->fields[i].value;
    }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetRecreate2 --
 *
 *      This is a faster version of Ns_SetRecreate() since it tries to reuse
 *      preallocated, but truncated Ns_Set structures. It saves potentially
 *      three ns_malloc operations:
 *        1) the Ns_Set structure
 *        2) the set name (it preserves the old name)
 *        3) the field set
 *      At the end content is copied and the from set is truncated.
 *
 * Results:
 *      new set.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_SetRecreate2(Ns_Set **toPtr, Ns_Set *from)
{
    Ns_Set      *newSet;

    NS_NONNULL_ASSERT(toPtr != NULL);
    NS_NONNULL_ASSERT(from != NULL);

    if (*toPtr == NULL) {
        /*
         * Everything has to e created, essentially the same happens in
         * Ns_SetRecreate()
         */
        newSet = ns_malloc(sizeof(Ns_Set));
        newSet->name = ns_strcopy(from->name);
        Ns_Log(Debug, "Ns_SetRecreate2: create a new set, new %" PRIuz "/%" PRIuz,
               from->size, from->maxSize);
        *toPtr = newSet;
        newSet->size = from->size;
        newSet->maxSize = from->maxSize;
        newSet->fields = ns_malloc(sizeof(Ns_SetField) * newSet->maxSize);
#ifdef NS_SET_DSTRING
        Tcl_DStringInit(&newSet->data);
#endif
    } else {
        newSet = *toPtr;
        /*
         * Keep always the old name.
         */
        assert(newSet->size == 0u);

        if (newSet->maxSize >= from->size) {
            /*
             * The old Ns_Set has enough space, there is no need to create new
             * fields.
             */
            Ns_Log(Debug, "Ns_SetRecreate2: keep the old set and fields, old %lu/%lu from %lu/%lu",
                   newSet->size, newSet->maxSize, from->size, from->maxSize);

        } else {
            /*
             * We have to grow the old Ns_Set, since it does not fit all the
             * entries that have to be stored.
             */
            Ns_Log(Debug, "Ns_SetRecreate2: keep the old set, make new fields old %"
                   PRIuz "/%" PRIuz " from %" PRIuz "/%" PRIuz,
                   newSet->size, newSet->maxSize, from->size, from->maxSize);
            newSet->maxSize = from->maxSize;
            ns_free(newSet->fields);
            newSet->fields = ns_malloc(sizeof(Ns_SetField) * newSet->maxSize);
        }
        newSet->size = from->size;
#ifdef NS_SET_DSTRING
        Tcl_DStringSetLength(&newSet->data, 0);
#endif
    }
    SetCopyElements("recreate2", from, newSet);
    from->size = 0u;
#ifdef NS_SET_DSTRING
    Tcl_DStringSetLength(&from->data, 0);
#endif
    return newSet;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetPrint --
 *
 *      Dump the contents of a set to stderr.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will write to stderr.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetPrint(const Ns_Set *set)
{
    size_t  i;

    NS_NONNULL_ASSERT(set != NULL);
    if (set->name != NULL) {
        fprintf(stderr, "%s:\n", set->name);
    }
    for (i = 0u; i < set->size; ++i) {
        if (set->fields[i].name == NULL) {
            fprintf(stderr, "\t(null) = ");
        } else {
            fprintf(stderr, "\t%s = ", set->fields[i].name);
        }
        if (set->fields[i].value == NULL) {
            fprintf(stderr, "(null)\n");
        } else {
            fprintf(stderr, "%s\n", set->fields[i].value);
        }
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
