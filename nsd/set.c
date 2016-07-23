/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */


/*
 * set.c --
 *
 *	Implements the Ns_Set data type.
 */

#include "nsd.h"
#

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIUpdate --
 *
 *	Remove a tuple and re-add it (case insensitive).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetIUpdate(Ns_Set *set, const char *key, const char *value)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    Ns_SetIDeleteKey(set, key);
    (void)Ns_SetPut(set, key, value);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetUpdate --
 *
 *	Remove a tuple and re-add it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetUpdate(Ns_Set *set, const char *key, const char *value)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    Ns_SetDeleteKey(set, key);
    (void)Ns_SetPut(set, key, value);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetCreate --
 *
 *	Initialize a new set.
 *
 * Results:
 *	A pointer to a new set.
 *
 * Side effects:
 *	Memory is allocated; free with Ns_SetFree.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_SetCreate(const char *name)
{
    Ns_Set *setPtr;

    setPtr = ns_malloc(sizeof(Ns_Set));
    setPtr->size = 0u;
    setPtr->maxSize = 10u;
    setPtr->name = ns_strcopy(name);
    setPtr->fields = ns_malloc(sizeof(Ns_SetField) * setPtr->maxSize);
    return setPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetFree --
 *
 *	Free a set and its associated data with ns_free.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will free both the Ns_Set structure AND its tuples.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetFree(Ns_Set *set)
{
    if (set != NULL) {
        size_t i;

        assert(set->size <  set->maxSize);

        for (i = 0u; i < set->size; ++i) {
            ns_free(set->fields[i].name);
            ns_free(set->fields[i].value);
        }
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
 *	Insert a tuple into an existing set.
 *
 * Results:
 *	The index number of the new tuple.
 *
 * Side effects:
 *	The key/value will be strdup'ed.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_SetPutSz(Ns_Set *set, const char *key, const char *value, ssize_t size)
{
    size_t index;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(set->size <  set->maxSize);

    index = set->size;
    set->size++;
    if (set->size >= set->maxSize) {
        set->maxSize = set->size * 2u;
        set->fields = ns_realloc(set->fields,
				 sizeof(Ns_SetField) * set->maxSize);
    }
    set->fields[index].name = ns_strdup(key);
    set->fields[index].value = ns_strncopy(value, size);
    
    return index;
}

size_t
Ns_SetPut(Ns_Set *set, const char *key, const char *value)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetPutSz(set, key, value, -1);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetUniqueCmp --
 *
 *	Using the comparison function, see if multiple keys match
 *	key.
 *
 * Results:
 *	NS_FALSE: multiple keys match key
 *	NS_TRUE: 0 or 1 keys match key.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetUniqueCmp(const Ns_Set *set, const char *key,
                int (*cmp) (CONST char *s1, CONST char *s2))
{
    size_t i;
    bool   found;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);

    found = NS_FALSE;
    for (i = 0u; i < set->size; ++i) {
        const char *name = set->fields[i].name;

        if ((name == NULL) || (((*cmp) (key, name)) == 0)) {

            if (found) {
                return NS_FALSE;
            }
            found = NS_TRUE;
        }
    }

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetFindCmp --
 *
 *	Returns the index of a tuple matching key, using a comparison
 *	function callback.
 *
 * Results:
 *	A tuple index or -1 if no matches.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetFindCmp(const Ns_Set *set, const char *key,
              int (*cmp) (const char *s1, const char *s2))
{
    size_t i;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);
    
    if (likely(key != NULL)) {
	for (i = 0u; i < set->size; i++) {
	    const char *name = set->fields[i].name;

	    if (likely(name != NULL) && ((*cmp) (key, name)) == 0) {
	      return (int)i;
	    }
	}
    } else {
	for (i = 0u; i < set->size; i++) {
	    if (unlikely(set->fields[i].name == NULL)) {
	      return (int)i;
	    }
	}
    }

    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetGetCmp --
 *
 *	Returns the value of a tuple matching key, using a comparison
 *	function callback.
 *
 * Results:
 *	A value or NULL if no matches.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_SetGetCmp(const Ns_Set *set, const char *key,
             int (*cmp) (const char *s1, const char *s2))
{
    int             i;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(cmp != NULL);

    i = Ns_SetFindCmp(set, key, cmp);
    if (i == -1) {
        return NULL;
    }
    return set->fields[i].value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetUnique --
 *
 *	Check if a key in a set is unique (case sensitive).
 *
 * Results:
 *	NS_TRUE if unique, NS_FALSE if not.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetUnique(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetUniqueCmp(set, key,
                           (int (*) (const char *left, const char *right)) strcmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIUnique --
 *
 *	Check if a key in a set is unique (case insensitive).
 *
 * Results:
 *	NS_TRUE if unique, NS_FALSE if not.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetIUnique(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetUniqueCmp(set, key,
                           (int (*) (const char *s1, const char *s2)) strcasecmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetFind --
 *
 *	Locate the index of a field in a set (case sensitive)
 *
 * Results:
 *	A field index or -1 if not found.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetFind(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetFindCmp(set, key,
                         (int (*) (const char *s1, const char *s2)) strcmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIFind --
 *
 *	Locate the index of a field in a set (case insensitive)
 *
 * Results:
 *	A field index or -1 if not found.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetIFind(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetFindCmp(set, key,
                         (int (*) (const char *s1, const char *s2)) strcasecmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetGet --
 *
 *	Return the value associated with a key, case sensitive.
 *
 * Results:
 *	A value or NULL if key not found.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_SetGet(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetGetCmp(set, key,
                        (int (*) (const char *s1, const char *s2)) strcmp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIGet --
 *
 *	Return the value associated with a key, case insensitive.
 *
 * Results:
 *	A value or NULL if key not found.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_SetIGet(const Ns_Set *set, const char *key)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    return Ns_SetGetCmp(set, key,
                        (int (*) (const char *s1, const char *s2)) strcasecmp);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetGetValue --
 *
 *      Return the value associated with a key, case sensitive.
 *      If no value found or it is empty, return provided default value
 *
 * Results:
 *      A value or NULL if key not found or default is NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SetGetValue(const Ns_Set *set, const char *key, const char *def)
{
    const char *value;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    value = Ns_SetGet(set, key);

    if (value == NULL || *value == '\0') {
	value = def;
    }
    return value;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetIGetValue --
 *
 *      Return the value associated with a key, case insensitive.
 *      If no value found or it is empty, return provided default value
 *
 * Results:
 *      A value or NULL if key not found or default is NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SetIGetValue(const Ns_Set *set, const char *key, const char *def)
{
    const char *value;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    value = Ns_SetIGet(set, key);

    if (value == NULL || *value == '\0') {
	value = def;
    }
    return value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetTrunc --
 *
 *	Remove all tuples after 'size'
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will free tuple memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetTrunc(Ns_Set *set, size_t size)
{
    NS_NONNULL_ASSERT(set != NULL);

    if (size < set->size) {
	size_t index;

        for (index = size; index < set->size; index++) {
            ns_free(set->fields[index].name);
            ns_free(set->fields[index].value);
        }
        set->size = size;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetDelete --
 *
 *	Delete a tuple from a set. If index is -1, this function does
 *	nothing.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will free tuple memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetDelete(Ns_Set *set, int index)
{
    NS_NONNULL_ASSERT(set != NULL);

    if ((index != -1) && (index < (int)set->size)) {
	size_t i;

        ns_free(set->fields[index].name);
        ns_free(set->fields[index].value);
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
 *	Set the value for a given tuple.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will free the old value dup the new value.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetPutValue(const Ns_Set *set, size_t index, const char *value)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    if (index < set->size) {
        ns_free(set->fields[index].value);
        set->fields[index].value = ns_strdup(value);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetDeleteKey --
 *
 *	Delete a tuple from the set (case sensitive).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will free tuple memory.
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
 *	Delete a tuple from the set (case insensitive).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will free tuple memory.
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
 *	In a null-terminated array of sets, find the set with the
 *	given name.
 *
 * Results:
 *	A set, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_SetListFind(Ns_Set *const*sets, const char *name)
{
    NS_NONNULL_ASSERT(sets != NULL);

    while (*sets != NULL) {
        if (name == NULL) {
            if ((*sets)->name == NULL) {
                return (*sets);
            }
        } else {
            if ((*sets)->name != NULL &&
		STREQ((*sets)->name, name)) {

                return (*sets);
            }
        }
        ++sets;
    }
    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetSplit --
 *
 *	Split a set into an array of new sets. This assumes that each
 *	key name in the fields of a set contains a separating
 *	character. The fields of the set are partitioned into new
 *	sets whose set names are the characters before the separator
 *	and whose field key names are the characters after the
 *	separator.
 *
 * Results:
 *	A new set.
 *
 * Side effects:
 *	Will allocate a new set and tuples.
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
    Ns_DStringNAppend(&ds, (char *) &end, (int)sizeof(Ns_Set *));

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
            Ns_DStringNAppend(&ds, (char *) &end, (int)sizeof(Ns_Set *));
        }
        (void)Ns_SetPut(next, key, set->fields[i].value);
        if (name != NULL) {
            *--key = sep;
        }
    }
    return (Ns_Set **) Ns_DStringExport(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetListFree --
 *
 *	Free a null-terminated array of sets.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will free all sets in the array and their tuples.
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
 * Ns_SetMerge --
 *
 *	Combine the 'low' set into the 'high' set.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will add tuples to 'high'.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetMerge(Ns_Set *high, const Ns_Set *low)
{
    size_t i;

    NS_NONNULL_ASSERT(high != NULL);
    NS_NONNULL_ASSERT(low != NULL);

    for (i = 0u; i < low->size; ++i) {
        int j = Ns_SetFind(high, low->fields[i].name);
        if (j == -1) {
            (void)Ns_SetPut(high, low->fields[i].name, low->fields[i].value);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetCopy --
 *
 *	Make a duplicate of a set.
 *
 * Results:
 *	A new set.
 *
 * Side effects:
 *	Will copy tuples and alloc new memory for them, too.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_SetCopy(const Ns_Set *old)
{
    size_t          i;
    Ns_Set         *new;

    if (old == NULL) {
        return NULL;
    }
    new = Ns_SetCreate(old->name);
    for (i = 0u; i < old->size; ++i) {
        (void)Ns_SetPut(new, old->fields[i].name, old->fields[i].value);
    }

    return new;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetMove --
 *
 *	Moves the data from one set to another, truncating the "from"
 *	set.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
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
 *	Combination of a create and a move operation. A new set is created,
 *	all data from the old set is moved to the new set, and the old set is
 *	truncated.
 *
 * Results:
 *	new set.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Ns_Set *
Ns_SetRecreate(Ns_Set *set)
{
    Ns_Set      *newSet;
    size_t       i;

    NS_NONNULL_ASSERT(set != NULL);

    newSet = ns_malloc(sizeof(Ns_Set));
    newSet->size = set->size;
    newSet->maxSize = set->maxSize;
    newSet->name = ns_strcopy(set->name);
    newSet->fields = ns_malloc(sizeof(Ns_SetField) * newSet->maxSize);
    
    for (i = 0u; i < set->size; i++) {
	newSet->fields[i].name  = set->fields[i].name;
        newSet->fields[i].value = set->fields[i].value;
    }
    set->size = 0u;
    
    return newSet;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SetPrint --
 *
 *	Dump the contents of a set to stderr.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will write to stderr.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetPrint(const Ns_Set *set)
{
    size_t  i;

    NS_NONNULL_ASSERT(set != NULL);

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
