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
 *----------------------------------------------------------------------
 *
 * Ns_DList  --  Dynamic list implementation for NaviServer
 *
 * Overview:
 *      Ns_DList provides a dynamic array implementation for managing lists
 *      of generic pointers ('void *'). Its interface and behavior are
 *      conceptually similar to Tcl's 'Tcl_DString' API, but instead of
 *      managing character data, it manages arbitrary pointer elements.
 *
 *        typedef struct Ns_DList {
 *            void   *data;
 *            size_t  size;
 *            size_t  avail;
 *            void   *static_data[30];
 *            Ns_FreeProc *freeProc;
 *        } Ns_DList;
 *
 *      Like 'Tcl_DString', Ns_DList starts with an internal static buffer
 *      of fixed size, avoiding heap allocations for small lists. When the
 *      number of elements exceeds this built-in capacity, Ns_DList
 *      transparently switches to heap-allocated storage. To minimize the
 *      number of reallocations, the capacity is automatically doubled
 *      when appending elements once the current allocation is exhausted.
 *
 * Ownership model:
 *      By default, Ns_DList treats its stored elements as plain pointers
 *      and does not free them when they are removed or the list is destroyed.
 *      However, a list can be configured to **own** its elements via:
 *
 *          Ns_DListSetFreeProc(&list, ns_free);
 *
 *      If a freeProc is set, it is automatically called on every element
 *      that is removed — via Ns_DListDelete(), Ns_DListSetLength(),
 *      Ns_DListReset(), or Ns_DListFree(). This is useful for lists of
 *      heap-allocated objects, such as:
 *
 *          - dynamically allocated strings
 *          - other heap-allocated resources tied to the list's lifecycle
 *
 *      For safety, Ns_DListSetFreeProc() may only be called on an empty
 *      list. Attempting to set it on a non-empty list logs a warning and
 *      leaves the existing freeProc unchanged.
 *
 * Key properties:
 *      - Static buffer optimization:
 *            Uses a fixed-size 'static_data[]' array for small lists.
 *      - Dynamic growth:
 *            Automatically switches to heap allocation when needed.
 *      - Doubling strategy:
 *            On expansion, capacity doubles to reduce reallocations,
 *            providing amortized O(1) append performance.
 *      - Optional ownership:
 *            Lists can manage the lifetime of their elements automatically
 *            via a freeProc, or behave as plain pointer containers.
 *      - Safe shrinking:
 *            Functions like 'Ns_DListSetCapacity()' can migrate back from
 *            heap storage into the static buffer when the size allows.
 *      - Pointer-based storage:
 *            Stores 'void *' elements; no ownership semantics are enforced
 *            unless explicitly documented.
 *
 * Interface summary:
 *
 *      Initialization & destruction:
 *          void Ns_DListInit(Ns_DList *dlPtr);
 *          void Ns_DListFree(Ns_DList *dlPtr);
 *          void Ns_DListSetFreeProc(Ns_DList *dlPtr, Ns_FreeProc freeProc);
 *
 *      Element management:
 *          void Ns_DListAppend(Ns_DList *dlPtr, void *element);
 *          bool Ns_DListAddUnique(Ns_DList *dlPtr, void *element);
 *          bool Ns_DListDelete(Ns_DList *dlPtr, void *element);
 *          char *Ns_DListSaveString(Ns_DList *dlPtr, const char *string);
 *
 *      Bulk freeing:
 *          void Ns_DListFreeElements(Ns_DList *dlPtr);
 *              (Equivalent to Ns_DListSetFreeProc(..., ns_free) + Reset)
 *
 *      Length & capacity management:
 *          void Ns_DListSetLength(Ns_DList *dlPtr, size_t size);
 *          void Ns_DListSetCapacity(Ns_DList *dlPtr, size_t capacity);
 *          size_t Ns_DListCapacity(const Ns_DList *dlPtr);
 *
 *      Legacy helpers (to be deprecated):
 *          char *Ns_DListSaveString(Ns_DList *dlPtr, const char *string);
 *          void  Ns_DListFreeElements(Ns_DList *dlPtr);
 *
 * Usage notes:
 *      - For lists of unmanaged pointers, use the default behavior and
 *        Ns_DListAppend().
 *      - For lists owning heap-allocated resources, set a freeProc **before**
 *        adding elements, and Ns_DList will handle cleanup automatically.
 *      - To truncate or extend the logical length explicitly, call
 *        Ns_DListSetLength(). When shrinking, removed elements are freed
 *        if a freeProc is set, and their slots are NULLed.
 *      - Ns_DListFree() releases all resources, including stored elements
 *        if ownership is enabled.
 *      - The helpers Ns_DListSaveString() and Ns_DListFreeElements() are
 *        kept for backward compatibility but new code should avoid them.
 *
 * Comparison with Tcl_DString:
 *      - Similar API semantics, but for 'void *' instead of 'char'.
 *      - Uses a doubling strategy for efficient growth, like Tcl's
 *        'Tcl_DString'.
 *      - Designed to avoid unnecessary heap allocations for small lists.
 *
 *
 *----------------------------------------------------------------------
 */

#include "nsd.h"

/*
 * Maximum number of elements that avail can represent
 */
#define NS_DLIST_MAX_ELEMENTS (SIZE_MAX / sizeof(void*))
/*
 *----------------------------------------------------------------------
 *
 * Ns_DListCapacity --
 *
 *      Return the total capacity of the dynamic list, i.e., the number of
 *      slots currently allocated for elements (including used and unused
 *      slots).
 *
 * Results:
 *      Returns the total capacity as a size_t.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
size_t
Ns_DListCapacity(const Ns_DList *dlPtr)
{
    return dlPtr->size + dlPtr->avail;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DListInit --
 *
 *      Initialize a dynamic list structure. The list starts out using
 *      the built-in static buffer. No heap memory is allocated until the
 *      number of elements exceeds the capacity of the static buffer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Resets size and available slots to initial values.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DListInit(Ns_DList *dlPtr)
{
    dlPtr->data = &dlPtr->static_data[0];
    dlPtr->avail = Ns_NrElements(dlPtr->static_data);
    dlPtr->size = 0u;
    dlPtr->freeProc = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListSetFreeProc --
 *
 *      Set an optional element cleanup function (deleter) for the dynamic
 *      list. If specified, the freeProc is called automatically whenever
 *      elements are removed from the list, either explicitly via
 *      Ns_DListDelete(), Ns_DListSetLength(), or implicitly when the
 *      list is freed.
 *
 *      For safety, the freeProc can only be set when the list is empty,
 *      since otherwise ownership semantics would become ambiguous.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Logs a warning and ignores the request if the list already contains
 *      elements.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DListSetFreeProc(Ns_DList *dlPtr, Ns_FreeProc freeProc)
{
    if (dlPtr->size != 0u) {
        Ns_Log(Warning, "dlist: cannot set freeProc for a list with elements; ignored");
    } else {
        dlPtr->freeProc = freeProc;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListFree --
 *
 *      Free any dynamically allocated memory associated with the list and
 *      reset it to its initial state. If the list only uses the static
 *      buffer, no heap memory is freed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May free heap memory.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DListFree(Ns_DList *dlPtr)
{
    if (dlPtr->freeProc != NULL) {
        Ns_DListFreeRange(dlPtr, 0, dlPtr->size, /*clear=*/false);
    }
    if (dlPtr->data != dlPtr->static_data) {
        ns_free(dlPtr->data);
    }
    Ns_DListInit(dlPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListFreeRange --
 *
 *      Free all elements in the specified half-open range [from, to_excl).
 *      If a freeProc is defined for the list, it is called for each element
 *      in the range before removal. If clear is true, the corresponding
 *      slots are zeroed out after freeing.
 *
 *      This helper is primarily used internally by Ns_DListSetLength(),
 *      Ns_DListFree(), and other operations that drop or truncate elements.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      - May call the list's freeProc on individual elements.
 *      - May write NULLs into the freed slots if clear == true.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DListFreeRange(Ns_DList *dlPtr, size_t from, size_t to_excl, bool clear)
{
    if (dlPtr->freeProc != NULL) {
        for (size_t i = from; i < to_excl; ++i) {
            if (dlPtr->data[i] != NULL) {
                dlPtr->freeProc(dlPtr->data[i]);
            }
        }
    }
    /* Then NULL-out the slots if requested */
    if (clear) {
        memset(&dlPtr->data[from], 0,
               (to_excl - from) * sizeof(dlPtr->data[0]));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ns_calc_array_bytes --
 *
 *      Safely compute the total byte size for an array of n elements
 *      of size "elem", with overflow and allocation sanity checks.
 *
 * Results:

 *      Returns 1 on success and stores the computed byte count in the output
 *      variable "*bytes". Returns 0 if an overflow occurs or if the result
 *      exceeds the conservative allocation bound (NS_MAXOBJ (roughly
 *      PTRDIFF_MAX).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#if (defined(__clang__) && defined(__has_builtin) && __has_builtin(__builtin_mul_overflow)) || \
    (defined(__GNUC__)  && !defined(__clang__)      && (__GNUC__ >= 5))
#  define NS_HAVE_BUILTIN_MUL_OVERFLOW 1
#endif

#ifndef NS_MAXOBJ
#  define NS_MAXOBJ PTRDIFF_MAX
#endif

static inline int
ns_calc_array_bytes(size_t n, size_t elem, size_t *bytes)
{
    size_t prod;

#ifdef NS_HAVE_BUILTIN_MUL_OVERFLOW
    if (__builtin_mul_overflow(n, elem, &prod)) {
        return 0;
    }
#else
    if (elem != 0 && n > (size_t)NS_MAXOBJ / elem) {
        return 0;
    }
    prod = n * elem;
#endif
    if (prod > (size_t)NS_MAXOBJ) {
        return 0;
    }
    *bytes = prod;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListSetCapacity --
 *
 *      Set the allocated capacity of the list to exactly the requested size,
 *      subject to the following rules:
 *         - The capacity is never reduced below the built-in static buffer.
 *         - The capacity is never smaller than the current size.
 *         - If the requested capacity fits within the static buffer and the
 *           list currently uses heap storage, the list is migrated back to
 *           static storage and heap memory is freed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May allocate or free heap memory.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DListSetCapacity(Ns_DList *dlPtr, size_t new_cap)
{
    const size_t size       = dlPtr->size;
    const size_t static_cap = Ns_NrElements(dlPtr->static_data);
    const size_t curr_cap   = Ns_DListCapacity(dlPtr);

    /* Capacity must never be smaller than size */
    if (new_cap < size) {
        new_cap = size;
    }

    /* Also, we never allocate less than static_cap slots */
    if (new_cap < static_cap) {
        new_cap = static_cap;
    }

    /* If the requested capacity equals current capacity, do nothing */
    if (new_cap == curr_cap) {
        dlPtr->avail = curr_cap - size;
        return;
    }

    /* Case 1: Move back into static_data if possible */
    if (new_cap == static_cap && size <= static_cap) {
        if (dlPtr->data != dlPtr->static_data) {
            /* Copy current elements into static storage */
            if (size > 0) {
                memcpy(dlPtr->static_data, dlPtr->data,
                       size * sizeof(dlPtr->data[0]));
            }
            ns_free(dlPtr->data);
            dlPtr->data = dlPtr->static_data;
        }
        dlPtr->avail = static_cap - size;
        return;
    }

    /* Case 2: Allocate or reallocate heap storage */
    if (new_cap > NS_DLIST_MAX_ELEMENTS) {
        Ns_Fatal("Ns_DListSetCapacity: capacity overflow");

    } else {
        void **newData;
        size_t bytes;

        if (!ns_calc_array_bytes(new_cap, sizeof dlPtr->data[0], &bytes)) {
            Ns_Log(Error, "Ns_DList: capacity overflow (need=%zu elems)", new_cap);
        } else {
            if (dlPtr->data == dlPtr->static_data) {
                /* First transition from static to heap */
                newData = ns_malloc(new_cap * sizeof(dlPtr->data[0]));
                if (size > 0) {
                    memcpy(newData, dlPtr->static_data,
                           size * sizeof(dlPtr->data[0]));
                }
            } else {
                /* Reallocate existing heap buffer */
                newData = ns_realloc(dlPtr->data,
                                     new_cap * sizeof(dlPtr->data[0]));
            }
            dlPtr->data  = newData;
            dlPtr->avail = new_cap - size;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DListSetLength --
 *
 *      Set the logical length of the list to the specified size. If the new
 *      size is larger than the current capacity, the capacity is increased
 *      exactly to the requested size. Shrinking the list does not release
 *      memory.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May allocate heap memory when growing.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DListSetLength(Ns_DList *dlPtr, size_t new_size)
{
    NS_NONNULL_ASSERT(dlPtr != NULL);

    if (new_size < dlPtr->size) {
        /* Free and clear dropped tail */
        Ns_DListFreeRange(dlPtr, new_size, dlPtr->size, /*clear=*/true);

    } else if (new_size > Ns_DListCapacity(dlPtr)) {
        /* Exact growth - no policy/headroom here */
        Ns_DListSetCapacity(dlPtr, new_size);
    }

    /* NULL is required when we have a freeProc defined */
    if (dlPtr->freeProc != NULL && new_size > dlPtr->size) {
        memset(&dlPtr->data[dlPtr->size], 0,
               (new_size - dlPtr->size) * sizeof(dlPtr->data[0]));
    }

    dlPtr->size  = new_size;
    dlPtr->avail = Ns_DListCapacity(dlPtr) - new_size;
}

void
Ns_DListReset(Ns_DList *dlPtr)
{
    Ns_DListSetLength(dlPtr, 0u); /* frees tail if owning */
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListAppend --
 *
 *      Append an element to the dynamic list. If the static buffer is full,
 *      the function automatically transitions to heap storage and doubles
 *      the capacity to minimize future reallocations.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May allocate or reallocate heap memory.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DListAppend(Ns_DList *dlPtr, void *element)
{
    NS_NONNULL_ASSERT(dlPtr != NULL);

    if (dlPtr->avail == 0) {
        size_t curr_cap = Ns_DListCapacity(dlPtr);
        size_t new_cap;

        /* Safe doubling; avoid size_t overflow */
        if (curr_cap > NS_DLIST_MAX_ELEMENTS / 2u) {
            /*
             * Can't double: grow minimally to avoid overflow of size_t. This
             * could still overflow size_t after some insanly long period, but
             * we assume that malloc will bail out before we reach 16 EiB on 64
             * bit machines.
             */
            new_cap = curr_cap + 1u;
        } else {
            new_cap = curr_cap * 2u;
        }

        Ns_DListSetCapacity(dlPtr, new_cap);
    }

    dlPtr->data[dlPtr->size++] = element;
    dlPtr->avail--;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListAddUnique --
 *
 *      Append an element to the dynamic list only if it is not already
 *      present. The function performs a linear search to check for
 *      duplicates and appends the element if it is not found.
 *
 * Results:
 *      Returns NS_TRUE if the element was newly added, NS_FALSE otherwise.
 *
 * Side effects:
 *      May allocate or reallocate heap memory if the element is new.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_DListAddUnique(Ns_DList *dlPtr, void *element)
{
    size_t i;

    for (i = 0u; i < dlPtr->size; i++) {
        if (dlPtr->data[i] == element) {
            return NS_FALSE;  /* already present */
        }
    }
    Ns_DListAppend(dlPtr, element);
    return NS_TRUE;  /* added */
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListDelete --
 *
 *      Search for the specified element in the dynamic list and remove it
 *      if found. All subsequent elements are shifted down by one position
 *      to keep the list compact, and the size and available slot counters
 *      are updated accordingly.
 *
 * Results:
 *      Returns NS_TRUE if the element was found and removed,
 *      NS_FALSE otherwise.
 *
 * Side effects:
 *      Modifies the list contents and may move elements in memory.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_DListDelete(Ns_DList *dlPtr, void *element)
{
    size_t i;

    for (i = 0; i < dlPtr->size; ++i) {
        if (dlPtr->data[i] == element) {
            /* free the element if we own it */
            if (dlPtr->freeProc != NULL && dlPtr->data[i] != NULL) {
                dlPtr->freeProc(dlPtr->data[i]);
            }
            /* compact */
            if (i + 1u < dlPtr->size) {
                memmove(&dlPtr->data[i],
                        &dlPtr->data[i + 1],
                        (dlPtr->size - i - 1) * sizeof(dlPtr->data[0]));
            }
            dlPtr->size--;
            dlPtr->avail++;
            dlPtr->data[dlPtr->size] = NULL;

            return NS_TRUE;
        }
    }
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListSaveString --
 *
 *      Legacy helper to duplicate a (potentially volatile) string using
 *      ns_strdup(), append the copy to the list, and return the copy.
 *
 *      New code should prefer setting a freeProc via
 *      Ns_DListSetFreeProc(&list, ns_free) and then explicitly
 *      duplicating strings before appending them to the list:
 *
 *          Ns_DListSetFreeProc(&list, ns_free);
 *          Ns_DListAppend(&list, ns_strdup(string));
 *
 *      This makes the ownership semantics explicit and avoids depending
 *      on this legacy convenience wrapper.
 *
 * Results:
 *      Returns the newly allocated copy, or NULL if the input string is NULL.
 *
 * Side effects:
 *      Allocates a copy of the input string and appends it to the list.
 *
 *----------------------------------------------------------------------
 */
char *
Ns_DListSaveString(Ns_DList *dlPtr, const char *string)
{
    char *result;

    if (string != NULL) {
        result = ns_strdup(string);
        Ns_DListAppend(dlPtr, result);
    } else {
        result = NULL;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListFreeElements --
 *
 *      Legacy helper to free every element in the list using ns_free()
 *      and then free the list's dynamic storage by calling Ns_DListFree().
 *
 *      New code should prefer setting a freeProc via
 *      Ns_DListSetFreeProc(&list, ns_free) and then using
 *      Ns_DListReset() or Ns_DListFree(), which handle element cleanup
 *      automatically.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees all elements using ns_free(), then resets the list.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DListFreeElements(Ns_DList *dlPtr)
{
    size_t element;

    for (element = 0; element < dlPtr->size; element ++) {
        ns_free((char*)dlPtr->data[element]);
    }
    Ns_DListFree(dlPtr);
}


#if 0
Ns_DList list;
Ns_DListInit(&list);

/* Append some items */
for (int i = 0; i < 50; i++) {
    Ns_DListAppend(&list, items[i]);
}

/* Reserve capacity for 100 entries */
Ns_DListSetCapacity(&list, 100);

/* Later, trim unused memory aggressively */
Ns_DListSetCapacity(&list, list.size);

/* Or migrate back to static storage if possible */
Ns_DListSetCapacity(&list, 0);
#endif
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
