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
 * index.c --
 *
 *      Implement the index data type.
 */

#include "nsd.h"

/*
 * Local functions defined in this file
 */

static ssize_t BinSearch(void *const*elPtrPtr, void *const* listPtrPtr, ssize_t n, Ns_IndexCmpProc *cmpProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static ssize_t BinSearchKey(const void *key, void *const*listPtrPtr, ssize_t n, Ns_IndexCmpProc *cmpProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static Ns_IndexCmpProc CmpStr;
static Ns_IndexKeyCmpProc CmpKeyWithStr;

static Ns_IndexCmpProc CmpInts;
static Ns_IndexKeyCmpProc CmpKeyWithInt;

#ifdef _MSC_VER_VERY_OLD
static void *
NsBsearch (register const void *key, register const void *base,
           register size_t nmemb, register size_t size,
           int (*compar)(const void *left, const void *right));
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexInit --
 *
 *      Initialize a new index.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will allocate space for the index elements from the heap.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexInit(Ns_Index *indexPtr, size_t inc,
             int (*CmpEls) (const void *left, const void *right),
             int (*CmpKeyWithEl) (const void *left, const void *right))
{

    NS_NONNULL_ASSERT(indexPtr != NULL);
    NS_NONNULL_ASSERT(CmpEls != NULL);
    NS_NONNULL_ASSERT(CmpKeyWithEl != NULL);

    indexPtr->n = 0u;
    indexPtr->max = inc;
    indexPtr->inc = inc;

    indexPtr->CmpEls = CmpEls;
    indexPtr->CmpKeyWithEl = CmpKeyWithEl;

    indexPtr->el = (void **) ns_malloc((size_t)inc * sizeof(void *));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexTrunc --
 *
 *      Remove all elements from an index.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees and mallocs element memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexTrunc(Ns_Index* indexPtr)
{
    NS_NONNULL_ASSERT(indexPtr != NULL);

    indexPtr->n = 0u;
    ns_free(indexPtr->el);
    indexPtr->max = indexPtr->inc;
    indexPtr->el = (void **) ns_malloc((size_t)indexPtr->inc * sizeof(void *));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexDestroy --
 *
 *      Release all of an index's memory.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees elements.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexDestroy(Ns_Index *indexPtr)
{
    NS_NONNULL_ASSERT(indexPtr != NULL);

    indexPtr->CmpEls = NULL;
    indexPtr->CmpKeyWithEl = NULL;
    ns_free(indexPtr->el);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexDup --
 *
 *      Make a copy of an index.
 *
 * Results:
 *      A pointer to a copy of the index.
 *
 * Side effects:
 *      Mallocs memory for index and elements.
 *
 *----------------------------------------------------------------------
 */

Ns_Index *
Ns_IndexDup(const Ns_Index *indexPtr)
{
    Ns_Index *newPtr;

    NS_NONNULL_ASSERT(indexPtr != NULL);

    newPtr = (Ns_Index *) ns_malloc(sizeof(Ns_Index));
    memcpy(newPtr, indexPtr, sizeof(Ns_Index));
    newPtr->el = (void **) ns_malloc(indexPtr->max * sizeof(void *));
    memcpy(newPtr->el, indexPtr->el, indexPtr->n * sizeof(void *));

    return newPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexFind --
 *
 *      Find a key in an index.
 *
 * Results:
 *      A pointer to the element, or NULL if none found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_IndexFind(const Ns_Index *indexPtr, const void *key)
{
    void *const *pPtrPtr;

    NS_NONNULL_ASSERT(indexPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    pPtrPtr = (void **) bsearch(key, indexPtr->el, indexPtr->n,
                                sizeof(void *), indexPtr->CmpKeyWithEl);

    return (pPtrPtr != NULL) ? *pPtrPtr : NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexFindInf --
 *
 *      Find the element with the key, or if none exists, the element
 *      before which the key would appear.
 *
 * Results:
 *      An element, or NULL if the key is not there AND would be the
 *      last element in the list.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_IndexFindInf(const Ns_Index *indexPtr, const void *key)
{
    void *result = NULL;

    NS_NONNULL_ASSERT(indexPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    if (indexPtr->n > 0u) {
        ssize_t i = BinSearchKey(key, indexPtr->el, (ssize_t)indexPtr->n,
                             indexPtr->CmpKeyWithEl);

        if (i < (ssize_t)indexPtr->n) {
            if ((i > 0) &&
                ((indexPtr->CmpKeyWithEl)(key, &(indexPtr->el[i])) != 0))  {
                result = indexPtr->el[i - 1];
            } else {
                result = indexPtr->el[i];
            }
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexFindMultiple --
 *
 *      Find all elements that match key.
 *
 * Results:
 *      An array of pointers to matching elements, terminated with a
 *      null pointer.
 *
 * Side effects:
 *      Will allocate memory for the return result.
 *
 *----------------------------------------------------------------------
 */

void **
Ns_IndexFindMultiple(const Ns_Index *indexPtr, const void *key)
{
    void *const  *firstPtrPtr;
    void        **retPtrPtr = NULL;

    NS_NONNULL_ASSERT(indexPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    /*
     * Find a place in the array that matches the key
     */
    firstPtrPtr = (void **) bsearch(key, indexPtr->el, indexPtr->n,
                                    sizeof(void *), indexPtr->CmpKeyWithEl);

    if (firstPtrPtr != NULL) {
        size_t i, n;

        /*
         * Search linearly back to make sure we've got the first one
         */

        while (firstPtrPtr != indexPtr->el
            && indexPtr->CmpKeyWithEl(key, firstPtrPtr - 1) == 0) {
            firstPtrPtr--;
        }

        /*
         * Search linearly forward to find out how many there are
         */
        n = indexPtr->n - (size_t)(firstPtrPtr - indexPtr->el);
        for (i = 1u;
             i < n && indexPtr->CmpKeyWithEl(key, firstPtrPtr + i) == 0;
             i++) {
            ;
        }

        /*
         * Build array of values to return
         */
        retPtrPtr = ns_malloc((i + 1u) * sizeof(void *));
        if (unlikely(retPtrPtr == NULL)) {
            Ns_Fatal("IndexFindMultiple: out of memory while allocating result");
        } else {
            memcpy(retPtrPtr, firstPtrPtr, i * sizeof(void *));
            retPtrPtr[i] = NULL;
        }
    }

    return retPtrPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * BinSearch --
 *
 *      Modified from BinSearch in K&R.
 *
 * Results:
 *      The position where an element should be inserted even if it
 *      does not already exist. "bsearch" will just return NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
BinSearch(void *const* elPtrPtr, void *const* listPtrPtr, ssize_t n, Ns_IndexCmpProc *cmpProc)
{
    ssize_t low = 0, high = n-1, mid = 0;

    NS_NONNULL_ASSERT(elPtrPtr != NULL);
    NS_NONNULL_ASSERT(listPtrPtr != NULL);
    NS_NONNULL_ASSERT(cmpProc != NULL);

    while (low <= high) {
        int cond;

        mid = (low + high) / 2;
        cond = (*cmpProc) (elPtrPtr, ((const unsigned char *const*)listPtrPtr) + mid);
        if (cond < 0) {
            high = mid - 1;
        } else if (cond > 0) {
            low = mid + 1;
        } else {
            return mid;
        }
    }
    return (high < mid) ? mid : low;
}


/*
 *----------------------------------------------------------------------
 *
 * BinSearchKey --
 *
 *      Like BinSearch, but takes a key instead of an element
 *      pointer.
 *
 * Results:
 *      See BinSearch.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
BinSearchKey(const void *key, void *const* listPtrPtr, ssize_t n, Ns_IndexCmpProc *cmpProc)
{
    ssize_t low = 0, high = n-1, mid = 0;

    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(listPtrPtr != NULL);
    NS_NONNULL_ASSERT(cmpProc != NULL);

    while (low <= high) {
        int cond;

        mid = (low + high) / 2;
        cond = (*cmpProc)(key, ((const unsigned char *const*)listPtrPtr) + mid);
        if (cond < 0) {
            high = mid - 1;
        } else if (cond > 0) {
            low = mid + 1;
        } else {
            return mid;
        }
    }
    return (high < mid) ? mid : low;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexAdd --
 *
 *      Add an element to an index.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May allocate extra element memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexAdd(Ns_Index *indexPtr, void *el)
{
    ssize_t i;

    NS_NONNULL_ASSERT(indexPtr != NULL);
    NS_NONNULL_ASSERT(el != NULL);

    if (indexPtr->n == indexPtr->max) {
        indexPtr->max += indexPtr->inc;
        indexPtr->el = (void **) ns_realloc(indexPtr->el,
                                            indexPtr->max * sizeof(void *));
    } else if (indexPtr->max == 0u) {
        indexPtr->max = indexPtr->inc;
        indexPtr->el = (void **) ns_malloc(indexPtr->max * sizeof(void *));
    }
    if (indexPtr->n > 0u) {
        i = BinSearch(&el, indexPtr->el, (ssize_t)indexPtr->n, indexPtr->CmpEls);
    } else {
        i = 0;
    }

    if (i < (int)indexPtr->n) {
        size_t j;

        for (j = indexPtr->n; (int)j > i; j--) {
            indexPtr->el[j] = indexPtr->el[j - 1u];
        }
    }
    indexPtr->el[i] = el;
    indexPtr->n++;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexDel --
 *
 *      Remove an element from an index.
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
Ns_IndexDel(Ns_Index *indexPtr, const void *el)
{
    size_t i, j;

    NS_NONNULL_ASSERT(indexPtr != NULL);
    NS_NONNULL_ASSERT(el != NULL);

    for (i = 0u; i < indexPtr->n; i++) {
        if (indexPtr->el[i] == el) {
            indexPtr->n--;
            if (i < indexPtr->n) {
                for (j = i; j < indexPtr->n; j++) {
                    indexPtr->el[j] = indexPtr->el[j + 1u];
                }
            }
            break;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexEl --
 *
 *      Find the i'th element of an index.
 *
 * Results:
 *      Element.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_IndexEl(const Ns_Index *indexPtr, size_t i)
{
    NS_NONNULL_ASSERT(indexPtr != NULL);

    return indexPtr->el[i];
}


/*
 *----------------------------------------------------------------------
 *
 * CmpStr --
 *
 *      Default comparison function.
 *
 * Results:
 *      See strcmp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpStr(const void *leftPtr, const void *rightPtr)
{
    NS_NONNULL_ASSERT(leftPtr != NULL);
    NS_NONNULL_ASSERT(rightPtr != NULL);

    return strcmp(*(const char**)leftPtr, *((const char**)rightPtr));
}


/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithStr --
 *
 *      Default comparison function, with key.
 *
 * Results:
 *      See strcmp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpKeyWithStr(const void *key, const void *elPtr)
{
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(elPtr != NULL);

    return strcmp((const char *)key, *(const char *const*)elPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringInit --
 *
 *      Initialize an index where the elements themselves are
 *      strings.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See Ns_IndexInit.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringInit(Ns_Index *indexPtr, size_t inc)
{
    NS_NONNULL_ASSERT(indexPtr != NULL);

    Ns_IndexInit(indexPtr, inc, CmpStr, CmpKeyWithStr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringDup --
 *
 *      Make a copy of an index, using ns_strdup to duplicate the
 *      keys.
 *
 * Results:
 *      A new index.
 *
 * Side effects:
 *      Will make memory copies of the elements.
 *
 *----------------------------------------------------------------------
 */

Ns_Index *
Ns_IndexStringDup(const Ns_Index *indexPtr)
{
    Ns_Index *newPtr;
    size_t    i;

    NS_NONNULL_ASSERT(indexPtr != NULL);

    newPtr = (Ns_Index *) ns_malloc(sizeof(Ns_Index));
    memcpy(newPtr, indexPtr, sizeof(Ns_Index));
    newPtr->el = (void **) ns_malloc(indexPtr->max * sizeof(void *));

    for (i = 0u; i < newPtr->n; i++) {
        newPtr->el[i] = ns_strdup(indexPtr->el[i]);
    }

    return newPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringAppend --
 *
 *      Append one index of strings to another.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will append to the first index.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringAppend(Ns_Index *addtoPtr, const Ns_Index *addfromPtr)
{
    size_t i;

    NS_NONNULL_ASSERT(addtoPtr != NULL);
    NS_NONNULL_ASSERT(addfromPtr != NULL);

    for (i = 0u; i < addfromPtr->n; i++) {
        Ns_IndexAdd(addtoPtr, ns_strdup(addfromPtr->el[i]));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringDestroy --
 *
 *      Free an index of strings.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See Ns_IndexDestroy.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringDestroy(Ns_Index *indexPtr)
{
    size_t i;

    NS_NONNULL_ASSERT(indexPtr != NULL);

    for (i = 0u; i < indexPtr->n; i++) {
        ns_free(indexPtr->el[i]);
    }

    Ns_IndexDestroy(indexPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringTrunc --
 *
 *      Remove all elements from an index of strings.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See Ns_IndexTrunc.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringTrunc(Ns_Index *indexPtr)
{
    size_t i;

    NS_NONNULL_ASSERT(indexPtr != NULL);

    for (i = 0u; i < indexPtr->n; i++) {
        ns_free(indexPtr->el[i]);
    }

    Ns_IndexTrunc(indexPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * CmpInts --
 *
 *      Default comparison function for an index of ints.
 *
 * Results:
 *      -1: left < right; 1: left > right; 0: left == right
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpInts(const void *leftPtr, const void *rightPtr)
{
    int result, left, right;

    NS_NONNULL_ASSERT(leftPtr != NULL);
    NS_NONNULL_ASSERT(rightPtr != NULL);

    left = *(const int *)leftPtr;
    right = *(const int *)rightPtr;

    if (left == right) {
        result = 0;
    } else {
        result = (left < right) ? -1 : 1;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithInt --
 *
 *      Default comparison function for an index of ints, with key.
 *
 * Results:
 *      -1: key < el; 1: key > el; 0: key == el
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpKeyWithInt(const void *keyPtr, const void *elPtr)
{
    int result, key, element;

    NS_NONNULL_ASSERT(keyPtr != NULL);
    NS_NONNULL_ASSERT(elPtr != NULL);

    key = *(const int *)keyPtr;
    element = *(const int *)elPtr;

    if (key == element) {
        result = 0;
    } else {
        result = key < element ? -1 : 1;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexIntInit --
 *
 *      Initialize an index whose elements will be integers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See Ns_IndexInit.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexIntInit(Ns_Index *indexPtr, size_t inc)
{
    NS_NONNULL_ASSERT(indexPtr != NULL);

    Ns_IndexInit(indexPtr, inc, CmpInts, CmpKeyWithInt);
}

#ifdef _MSC_VER_VERY_OLD
#define bsearch(a, b, c, d, e) NsBsearch((a), (b), (c), (d), (e))

/*
 *----------------------------------------------------------------------
 *
 * NsBsearch --
 *
 *      Binary search.
 *
 *  Due to Windows hanging in its own bsearch() routine, this was added
 *  to alleviate the problem.
 *
 * Results:
 *      A pointer to the element, or NULL if none found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void *
NsBsearch (register const void *key, register const void *base,
           register size_t nmemb, register size_t size,
           int (*compar)(const void *key, const void *value))
{
    while (nmemb > 0) {
        register const void *mid_point;
        register int         cmp;

        mid_point = (char *)base + size * (nmemb >> 1);
        if ((cmp = (*compar)(key, mid_point)) == 0)
            return (void *)mid_point;
        if (cmp >= 0) {
            base  = (char *)mid_point + size;
            nmemb = (nmemb - 1) >> 1;
        } else
            nmemb >>= 1;
    }
    return NULL;
}
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
