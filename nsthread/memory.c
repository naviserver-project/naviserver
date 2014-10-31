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
 *	Memory allocation routines.
 */

#include "thread.h"


/*
 *----------------------------------------------------------------------
 *
 * ns_realloc, ns_malloc, ns_calloc, ns_free, ns_strdup, ns_strcopy --
 *
 *	Memory allocation wrappers which either call the platform
 *	versions or the fast pool allocator for a per-thread pool.
 *
 * Results:
 *	As with system functions.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
/* #define SYSTEM_MALLOC 1 */

#if defined(SYSTEM_MALLOC)
void *ns_realloc(void *ptr, size_t size)  { return realloc(ptr, size); }
void *ns_malloc(size_t size)              { return malloc(size); }
void ns_free(void *ptr)                   { /*fprintf(stderr, "free %p\n", ptr); if (ptr != NULL) */ {free(ptr);} }
void *ns_calloc(size_t num, size_t esize) { return calloc(num, esize); }
#else
void *
ns_realloc(void *ptr, size_t size)
{
  return (ptr ? ckrealloc(ptr, (int)size) : ckalloc((int)size));
}

void *
ns_malloc(size_t size)
{
    return ckalloc((int)size);
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
#endif

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
        size = likely(size > 0) ? size : strlen(old);
        new = ns_malloc(size + 1U);
        strncpy(new, old, size);
        new[size] = 0;
    }
    return new;
}

char *
ns_strdup(const char *old)
{
    char *new;

    new = ns_malloc(strlen(old) + 1U);
    strcpy(new, old);

    return new;
}
