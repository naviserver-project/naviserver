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
 * index.c --
 *
 *	Implement the index data type. 
 */

#include "nsd.h"

/*
 * Local functions defined in this file
 */

static int BinSearch(void *const*elPtrPtr, void *const* listPtrPtr, int n, Ns_IndexCmpProc *cmpProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static int BinSearchKey(const void *key, void *const*listPtrPtr, int n, Ns_IndexCmpProc *cmpProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static int CmpStr(const char *const*leftPtr, const char *const*rightPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int CmpKeyWithStr(const char *key, const char *const*elPtr)       NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int CmpInts(const int *leftPtr, const int *rightPtr)    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int CmpKeyWithInt(const int *keyPtr, const int *elPtr)  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

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
 *	Initialize a new index. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	Will allocate space for the index elements from the heap. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexInit(Ns_Index *indexPtr, int inc,
	     int (*CmpEls) (const void *left, const void *right),
	     int (*CmpKeyWithEl) (const void *left, const void *right))
{

    assert(indexPtr != NULL);
    assert(CmpEls != NULL);
    assert(CmpKeyWithEl != NULL);
    
    indexPtr->n = 0;
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
 *	Remove all elements from an index. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	Frees and mallocs element memory. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexTrunc(Ns_Index* indexPtr)
{
    assert(indexPtr != NULL);
    
    indexPtr->n = 0;
    ns_free(indexPtr->el);
    indexPtr->max = indexPtr->inc;
    indexPtr->el = (void **) ns_malloc((size_t)indexPtr->inc * sizeof(void *));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexDestroy --
 *
 *	Release all of an index's memory. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	Frees elements. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexDestroy(Ns_Index *indexPtr)
{
    assert(indexPtr != NULL);
    
    indexPtr->CmpEls = NULL;
    indexPtr->CmpKeyWithEl = NULL;
    ns_free(indexPtr->el);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexDup --
 *
 *	Make a copy of an index. 
 *
 * Results:
 *	A pointer to a copy of the index. 
 *
 * Side effects:
 *	Mallocs memory for index and elements. 
 *
 *----------------------------------------------------------------------
 */

Ns_Index *
Ns_IndexDup(const Ns_Index *indexPtr)
{
    Ns_Index *newPtr;

    assert(indexPtr != NULL);

    newPtr = (Ns_Index *) ns_malloc(sizeof(Ns_Index));
    memcpy(newPtr, indexPtr, sizeof(Ns_Index));
    newPtr->el = (void **) ns_malloc((size_t)indexPtr->max * sizeof(void *));
    memcpy(newPtr->el, indexPtr->el, (size_t)indexPtr->n * sizeof(void *));

    return newPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexFind --
 *
 *	Find a key in an index. 
 *
 * Results:
 *	A pointer to the element, or NULL if none found. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

void *
Ns_IndexFind(const Ns_Index *indexPtr, const void *key)
{
    void **pPtrPtr;

    assert(indexPtr != NULL);
    assert(key != NULL);

    pPtrPtr = (void **) bsearch(key, indexPtr->el, (size_t)indexPtr->n, 
                                sizeof(void *), indexPtr->CmpKeyWithEl);

    return (pPtrPtr != NULL) ? *pPtrPtr : NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexFindInf --
 *
 *	Find the elment with the key, or if none exists, the element 
 *	before which the key would appear. 
 *
 * Results:
 *	An element, or NULL if the key is not there AND would be the 
 *	last element in the list. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

void *
Ns_IndexFindInf(const Ns_Index *indexPtr, const void *key)
{
    assert(indexPtr != NULL);
    assert(key != NULL);
  
    if (indexPtr->n > 0) {
        int i;

        i = BinSearchKey(key, indexPtr->el, indexPtr->n,
			 indexPtr->CmpKeyWithEl);

        if (i >= indexPtr->n) {
          return NULL;
        }

        if ((i > 0) && \
            ((indexPtr->CmpKeyWithEl)(key, &(indexPtr->el[i])) != 0))  {
            return indexPtr->el[i - 1];
        } else {
            return indexPtr->el[i];
        }
    } else {
        return NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexFindMultiple --
 *
 *	Find all elements that match key. 
 *
 * Results:
 *	An array of pointers to matching elements, termianted with a 
 *	null poitner. 
 *
 * Side effects:
 *	Will allocate memory for the return result. 
 *
 *----------------------------------------------------------------------
 */

void **
Ns_IndexFindMultiple(const Ns_Index *indexPtr, const void *key)
{
    void **firstPtrPtr;

    assert(indexPtr != NULL);
    assert(key != NULL);

    /*
     * Find a place in the array that matches the key
     */
    
    firstPtrPtr = (void **) bsearch(key, indexPtr->el, (size_t)indexPtr->n,
				    sizeof(void *), indexPtr->CmpKeyWithEl);

    if (firstPtrPtr == NULL) {
        return NULL;
    } else {
        size_t i, n;
        void **retPtrPtr;

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
        for (i = 1u; i < n &&
                 indexPtr->CmpKeyWithEl(key, firstPtrPtr + i) == 0; i++) {
	    ;
	}
        /*
	 * Build array of values to return
	 */
	
        retPtrPtr = ns_malloc((i + 1u) * sizeof(void *));
        memcpy(retPtrPtr, firstPtrPtr, i * sizeof(void *));
        retPtrPtr[i] = NULL;

        return retPtrPtr;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * BinSearch --
 *
 *	Modified from BinSearch in K&R. 
 *
 * Results:
 *	The position where an element should be inserted even if it 
 *	does not already exist. bsearch will just return NULL. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

static int
BinSearch(void *const* elPtrPtr, void *const* listPtrPtr, int n, Ns_IndexCmpProc *cmpProc)
{
    int low = 0, high = n-1, mid = 0;

    assert(elPtrPtr != NULL);
    assert(listPtrPtr != NULL);
    assert(cmpProc != NULL);

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
 *	Like BinSearch, but takes a key instead of an element 
 *	pointer. 
 *
 * Results:
 *	See BinSearch. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

static int
BinSearchKey(const void *key, void *const* listPtrPtr, int n, Ns_IndexCmpProc *cmpProc)
{
    int low = 0, high = n-1, mid = 0;

    assert(key != NULL);
    assert(listPtrPtr != NULL);
    assert(cmpProc != NULL);

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
 *	Add an element to an index. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	May allocate extra element memory. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexAdd(Ns_Index *indexPtr, void *el)
{
    int i;

    assert(indexPtr != NULL);
    assert(el != NULL);

    if (indexPtr->n == indexPtr->max) {
        indexPtr->max += indexPtr->inc;
        indexPtr->el = (void **) ns_realloc(indexPtr->el,
					    (size_t)indexPtr->max * sizeof(void *));
    } else if (indexPtr->max == 0) {
        indexPtr->max += indexPtr->inc;
        indexPtr->el = (void **) ns_malloc((size_t)indexPtr->max * sizeof(void *));
    }
    if (indexPtr->n > 0) {
        i = BinSearch(&el, indexPtr->el, indexPtr->n, indexPtr->CmpEls);
    } else {
        i = 0;
    }

    if (i < indexPtr->n) {
        int j;
        for (j = indexPtr->n; j > i; j--) {
            indexPtr->el[j] = indexPtr->el[j - 1];
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
 *	Remove an element from an index. 
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
Ns_IndexDel(Ns_Index *indexPtr, const void *el)
{
    int i, j, done;

    assert(indexPtr != NULL);
    assert(el != NULL);

    done = 0;
    for (i = 0; i < indexPtr->n && done == 0; i++) {
        if (indexPtr->el[i] == el) {
            indexPtr->n--;
            if (i < indexPtr->n) {
                for (j = i; j < indexPtr->n; j++) {
                    indexPtr->el[j] = indexPtr->el[j + 1];
                }
            }
            done = 1;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexEl --
 *
 *	Find the i'th element of an index. 
 *
 * Results:
 *	Element. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

void *
Ns_IndexEl(const Ns_Index *indexPtr, int i)
{
    assert(indexPtr != NULL);

    return indexPtr->el[i];
}


/*
 *----------------------------------------------------------------------
 *
 * CmpStr --
 *
 *	Default comparison function. 
 *
 * Results:
 *	See strcmp. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

static int
CmpStr(const char *const*leftPtr, const char *const*rightPtr)
{
    assert(leftPtr != NULL);
    assert(rightPtr != NULL);

    return strcmp(*leftPtr, *rightPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithStr --
 *
 *	Default comparison function, with key. 
 *
 * Results:
 *	See strcmp. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

static int
CmpKeyWithStr(const char *key, const char *const*elPtr)
{
    assert(key != NULL);
    assert(elPtr != NULL);

    return strcmp(key, *elPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringInit --
 *
 *	Initialize an index where the elements themselves are 
 *	strings. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	See Ns_IndexInit. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringInit(Ns_Index *indexPtr, int inc)
{
    assert(indexPtr != NULL);

    Ns_IndexInit(indexPtr, inc, 
		 (int (*) (const void *left, const void *right)) CmpStr,
		 (int (*) (const void *left, const void *right)) CmpKeyWithStr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringDup --
 *
 *	Make a copy of an index, using ns_strdup to duplicate the 
 *	keys. 
 *
 * Results:
 *	A new index. 
 *
 * Side effects:
 *	Wil make memory copies of the elements. 
 *
 *----------------------------------------------------------------------
 */

Ns_Index *
Ns_IndexStringDup(const Ns_Index *indexPtr)
{
    Ns_Index *newPtr;
    int       i;

    assert(indexPtr != NULL);

    newPtr = (Ns_Index *) ns_malloc(sizeof(Ns_Index));
    memcpy(newPtr, indexPtr, sizeof(Ns_Index));
    newPtr->el = (void **) ns_malloc((size_t)indexPtr->max * sizeof(void *));

    for (i = 0; i < newPtr->n; i++) {
        newPtr->el[i] = ns_strdup(indexPtr->el[i]);
    }

    return newPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringAppend --
 *
 *	Append one index of strings to another. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	Will append to the first index.
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringAppend(Ns_Index *addtoPtr, const Ns_Index *addfromPtr)
{
    int i;

    assert(addtoPtr != NULL);
    assert(addfromPtr != NULL);

    for (i = 0; i < addfromPtr->n; i++) {
        Ns_IndexAdd(addtoPtr, ns_strdup(addfromPtr->el[i]));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringDestroy --
 *
 *	Free an index of strings. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	See Ns_IndexDestroy. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringDestroy(Ns_Index *indexPtr)
{
    int i;

    assert(indexPtr != NULL);
    
    for (i = 0; i < indexPtr->n; i++) {
        ns_free(indexPtr->el[i]);
    }

    Ns_IndexDestroy(indexPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexStringTrunc --
 *
 *	Remove all elements from an index of strings. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	See Ns_IndexTrunc. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexStringTrunc(Ns_Index *indexPtr)
{
    int i;

    assert(indexPtr != NULL);

    for (i = 0; i < indexPtr->n; i++) {
        ns_free(indexPtr->el[i]);
    }

    Ns_IndexTrunc(indexPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * CmpInts --
 *
 *	Default comparison function for an index of ints. 
 *
 * Results:
 *	-1: left < right; 1: left > right; 0: left == right 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

static int
CmpInts(const int *leftPtr, const int *rightPtr)
{

    assert(leftPtr != NULL);
    assert(rightPtr != NULL);

    if (*leftPtr == *rightPtr) {
        return 0;
    } else {
        return *leftPtr < *rightPtr ? -1 : 1;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithInt --
 *
 *	Default comparison function for an index of ints, with key. 
 *
 * Results:
 *	-1: key < el; 1: key > el; 0: key == el 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

static int
CmpKeyWithInt(const int *keyPtr, const int *elPtr)
{
    assert(keyPtr != NULL);
    assert(elPtr != NULL);

    if (*keyPtr == *elPtr) {
        return 0;
    } else {
        return *keyPtr < *elPtr ? -1 : 1;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IndexIntInit --
 *
 *	Initialize an index whose elements will be integers. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	See Ns_IndexInit. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_IndexIntInit(Ns_Index *indexPtr, int inc)
{
    assert(indexPtr != NULL);
    
    Ns_IndexInit(indexPtr, inc, 
		 (int (*) (const void *left, const void *right)) CmpInts,
		 (int (*) (const void *left, const void *right)) CmpKeyWithInt);
}

#ifdef _MSC_VER_VERY_OLD
#define bsearch(a,b,c,d,e) NsBsearch((a),(b),(c),(d),(e))

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
