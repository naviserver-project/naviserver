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
 * nscheck.h --
 *
 *      Stronger compile time error checking when using GCC.
 *
 */

#ifndef NSCHECK_H
#define NSCHECK_H

#undef  __GNUC_PREREQ
#if defined __GNUC__ && defined __GNUC_MINOR__
# define __GNUC_PREREQ(maj, min) \
        ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#else
# define __GNUC_PREREQ(maj, min) (0)
#endif


#if __GNUC_PREREQ(3,5)
# define NS_GNUC_SENTINEL __attribute__((__sentinel__))
#else
# define NS_GNUC_SENTINEL
#endif


#if __GNUC_PREREQ(3,4)
# define NS_GNUC_WARN_UNUSED_RESULT __attribute__((__warn_unused_result__))
#else
# define NS_GNUC_WARN_UNUSED_RESULT
#endif


#if __GNUC_PREREQ(3,3)
# define NS_GNUC_NONNULL(...) __attribute__((__nonnull__(__VA_ARGS__)))
# define NS_GNUC_MAYALIAS __attribute__((__may_alias__))
#else
# define NS_GNUC_NONNULL(...)
# define NS_GNUC_MAYALIAS
#endif


#if __GNUC_PREREQ(3,2)
# define NS_GNUC_DEPRECATED __attribute__((__deprecated__))
#else
# define NS_GNUC_DEPRECATED
#endif

#if __GNUC_PREREQ(4,5)
# define NS_GNUC_DEPRECATED_FOR(f) __attribute__((deprecated("Use " #f " instead")))
#else
# define NS_GNUC_DEPRECATED_FOR(f) NS_GNUC_DEPRECATED
#endif

#if __GNUC_PREREQ(3,1)
# define NS_GNUC_USED __attribute__((__used__))
#else
# define NS_GNUC_USED
#endif


#if __GNUC_PREREQ(2,8)
# define NS_GNUC_FORMAT(m) __attribute__((__format_arg__ (m)))
#else
# define NS_GNUC_FORMAT(m)
#endif


#if __GNUC_PREREQ(2,7)
# define NS_GNUC_UNUSED __attribute__((unused))
# define NS_GNUC_NORETURN __attribute__((__noreturn__))
# define NS_GNUC_PRINTF(m, n) __attribute__((__format__ (__printf__, m, n))) NS_GNUC_NONNULL(m)
# define NS_GNUC_SCANF(m, n) __attribute__((__format__ (__scanf__, m, n))) NS_GNUC_NONNULL(m)
#else
# define NS_GNUC_UNUSED
# define NS_GNUC_NORETURN
# define NS_GNUC_PRINTF(fmtarg, firstvararg)
# define NS_GNUC_SCANF(fmtarg, firstvararg)
#endif

/*
 * Tries to use gcc __attribute__ unused and mangles the name, so the
 * attribute could not be used, if declared as unused.
 */
#ifdef UNUSED
#elif __GNUC_PREREQ(2, 7)
# define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ (x)
#else
# define UNUSED(x) (x)
#endif

#if __GNUC_PREREQ(2,96)
# define NS_GNUC_MALLOC __attribute__((__malloc__))
# define NS_GNUC_CONST __attribute__((__const__))
#else
# define NS_GNUC_MALLOC
# define NS_GNUC_CONST
#endif

#if __GNUC_PREREQ(4, 9)
# define NS_GNUC_RETURNS_NONNULL __attribute__((returns_nonnull))
# define NS_GNUC_PURE __attribute__((__pure__))
# define NS_ALLOC_SIZE1(ARG) __attribute__((__alloc_size__(ARG)))
# define NS_ALLOC_SIZE2(ARG1,ARG2) __attribute__((__alloc_size__(ARG1,ARG2)))
#else
# define NS_GNUC_RETURNS_NONNULL
# define NS_GNUC_PURE
# define NS_ALLOC_SIZE1(ARG)
# define NS_ALLOC_SIZE2(ARG1,ARG2)
#endif


/*
 * In earlier version of GCC6, it complained when there was a nonnull
 * declaration on the argument followed by an assert checking the
 * argument for NULL that this is a redundant check. This note is left
 * here as a reference in case other compilers take similar roads.
 *
 * #if __GNUC_PREREQ(6, 0)
 * # define NS_NONNULL_ASSERT(assertion)
 * #endif
 */
#define NS_NONNULL_ASSERT(assertion) assert((assertion))

#if __GNUC_PREREQ(7, 0)
# define NS_FALL_THROUGH ;__attribute__((fallthrough))
#else
# define NS_FALL_THROUGH ((void)0)
#endif

/*
 * We include here limits.h, since this file includes <features.h>,
 * which should not be included directly by applications, but which is
 * needed for the *GLIBC* macros.
 */
#include <limits.h>

/*
 * On Solaris, the above might have defined _STRICT_STDC
 * but this makes problems with <signal.h> which does not
 * define "struct sigaction" any more. Quick and dirty
 * "solution" is simply to undef this thing. I do not know
 * what side-effects this brings and I do not care.
 */

#if defined(__sun__) && defined(_STRICT_STDC)
# undef _STRICT_STDC
#endif

#if defined(__GLIBC__) && defined(__GLIBC_MINOR__)
# if (__GLIBC__ > 2) || ((__GLIBC__ == 2) && (__GLIBC_MINOR__ >= 6))
#  define NS_FOPEN_SUPPORTS_MODE_E 1
# endif
#endif


#ifdef _MSC_VER
# define NS_INLINE __forceinline
# define NS_THREAD_LOCAL __declspec(thread)
#else
# define NS_INLINE inline
#endif

#if defined(__cplusplus)
# define NS_RESTRICT
#else
# ifdef _MSC_VER
#  define NS_RESTRICT __restrict
# else
#  define NS_RESTRICT restrict
# endif
#endif

#if defined(__GNUC__) && !defined(__OpenBSD__)
# define NS_THREAD_LOCAL __thread
#elif defined(__clang__)
# define NS_THREAD_LOCAL __thread
#elif defined NS_HAVE_C11
# include <threads.h>
# define NS_THREAD_LOCAL thread_local
#endif

#endif /* NSCHECK_H */
