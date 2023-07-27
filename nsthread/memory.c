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
 * memory.c --
 *
 *      Memory allocation routines.
 */

#include "thread.h"
/* #define NS_VERBOSE_MALLOC 1 */


/*
 *----------------------------------------------------------------------
 *
 * ns_realloc, ns_malloc, ns_calloc, ns_free, ns_strdup, ns_strcopy --
 *
 *      Memory allocation wrappers which either call the platform
 *      versions or the fast pool allocator for a per-thread pool.
 *
 * Results:
 *      As with system functions.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
/* #define SYSTEM_MALLOC 1 */

#if defined(SYSTEM_MALLOC)
void *ns_realloc(void *ptr, size_t size)  {
    void *result;

# ifdef NS_VERBOSE_MALLOC
    fprintf(stderr, "#MEM# realloc %lu\n", size);
# endif
    result = realloc(ptr, size);
    if (result == NULL) {
        fprintf(stderr, "Fatal: failed to reallocate %" PRIuz " bytes.\n", size);
        abort();
    }
    return result;
}
void *ns_malloc(size_t size) {
    void *result;
# ifdef NS_VERBOSE_MALLOC
    fprintf(stderr, "#MEM# malloc %lu\n", size);
# endif

    /*
     * In case of size == 0, the allowed result of a malloc call is either
     * NULL or a pointer to zero allocated bytes. Therefore, we cannot deduce
     * in general, that a malloc() result of NULL means out of memory.
     */
    result = malloc(size);
    /*if (size == 0u) {
        fprintf(stderr, "ZERO ns_malloc size=%lu ptr %p\n", size, result);
        }*/
    if (unlikely(result == NULL && size > 0u)) {
        fprintf(stderr, "Fatal: failed to allocate %" PRIuz " bytes.\n", size);
        abort();
    }
    return result;
}
void ns_free(void *ptr) {
    free(ptr);
}
void *ns_calloc(size_t num, size_t esize) {
    void *result;

    assert(num > 0u);
    assert(esize > 0u);

# ifdef NS_VERBOSE_MALLOC
    fprintf(stderr, "#MEM# calloc %lu\n", esize);
# endif

    result = calloc(num, esize);
    if (result == NULL) {
        fprintf(stderr, "Fatal: failed to allocate %" PRIuz " bytes.\n", num * esize);
        abort();
    }
    return result;
}
#else /* defined(SYSTEM_MALLOC) */
void *
ns_realloc(void *ptr, size_t size)
{
  return ((ptr != NULL) ? ckrealloc(ptr, (unsigned int)size) : ckalloc((unsigned int)size));
}

void *
ns_malloc(size_t size)
{
    return ckalloc((unsigned int)size);
}

void
ns_free(void *ptr)
{
    if (likely(ptr != NULL)) {
        ckfree(ptr);
    }
}

void *
ns_calloc(size_t num, size_t esize)
{
    void *new;
    size_t size;

    size = num * esize;
    new = ns_malloc(size);
    memset(new, 0, size);

    return new;
}
#endif /* defined(SYSTEM_MALLOC) */

char *
ns_strcopy(const char *old)
{
    return (old == NULL ? NULL : ns_strdup(old));
}

char *
ns_strncopy(const char *old, ssize_t size)
{
    char *new = NULL;

    if (likely(old != NULL)) {
        size_t new_size = likely(size > 0) ? (size_t)size : strlen(old);
        new_size ++;
        new = ns_malloc(new_size);
        if (new != NULL) {
            memcpy(new, old, new_size);
        } else {
#if defined(ENOMEM)
            errno = ENOMEM;
#endif
        }
    }
    return new;
}

char *
ns_strdup(const char *old)
{
    size_t length;
    char *p;

    NS_NONNULL_ASSERT(old != NULL);

    length = strlen(old) + 1u;
    p = ns_malloc(length);
    if (p != NULL) {
        memcpy(p, old, length);
    } else {
#if defined(ENOMEM)
        errno = ENOMEM;
#endif
    }

    return p;
}


/*
 *----------------------------------------------------------------------
 *
 * ns_uint32toa, ns_uint64toa --
 *
 *      This procedure formats an uint32_t or uint62_t into a sequence of
 *      decimal digits in a buffer. It is the caller's responsibility to
 *      ensure that enough storage is available. This procedure has the effect
 *      of snprintf(buffer, size, "%...d", n) but is substantially faster
 *
 * Results:
 *      Length of the written digits, not including the terminating "\0".
 *
 * Side effects:
 *      The formatted characters are written into the storage pointer to by
 *      the "buffer" argument.
 *
 *----------------------------------------------------------------------
 */

int
ns_uint32toa(
    char *buffer,  /* Points to the storage into which the
                    * formatted characters are written. */
    uint32_t n)    /* The value to be converted. */
{
    char            temp[TCL_INTEGER_SPACE];
    register char  *p = temp;
    int             len = 0;

    /*
     * Compute the digits.
     */
    do {
        *p++ = (char)((n % 10u) + UCHAR('0'));
        n /= 10u;
        len ++;
    } while (likely(n > 0u));

    /*
     * Reverse the digits.
     */
    do {
        *buffer++ = *--p;
    } while (likely(p != temp));

    /*
     * Terminate the string
     */
    *buffer = '\0';

    return len;
}

int
ns_uint64toa(
    char *buffer,  /* Points to the storage into which the
                    * formatted characters are written. */
    uint64_t n)    /* The value to be converted. */
{
    char            temp[TCL_INTEGER_SPACE];
    register char  *p = temp;
    int             len = 0;

    /*
     * Compute the digits.
     */
    do {
        *p++ = (char)((n % 10u) + UCHAR('0'));
        n /= 10u;
        len ++;
    } while (likely(n > 0u));

    /*
     * Reverse the digits.
     */
    do {
        *buffer++ = *--p;
    } while (likely(p != temp));

    /*
     * Terminate the string
     */
    *buffer = '\0';

    return len;
}
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
