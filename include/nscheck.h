/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
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
 * nscheck.h --
 *      
 *      Stronger compile time error checking when using GCC.
 *
 * $Header$
 */

#ifndef NSCHECK_H
#define NSCHECK_H

#ifndef _WIN32

#undef  __GNUC_PREREQ
#if defined __GNUC__ && defined __GNUC_MINOR__
# define __GNUC_PREREQ(maj, min) \
        ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#endif


#if __GNUC_PREREQ(3,5)
# define NS_GNUC_SENTINEL __attribute__((__sentinel__))
#else
# define NS_GNUC_SENTINEL
#endif


#if __GNUC_PREREQ(3,3)
# define NS_GNUC_NONNULL(...) __attribute__((__nonnull__(__VA_ARGS__)))
# define NS_GNUC_WARN_UNUSED_RESULT __attribute__((__warn_unused_result__))
# define NS_GNUC_MAYALIAS __attribute__((__may_alias__))
#else
# define NS_GNUC_NONNULL(...)
# define NS_GNUC_WARN_UNUSED_RESULT
# define NS_GNUC_MAYALIAS
#endif


#if __GNUC_PREREQ(3,2)
# define NS_GNUC_DEPRECATED __attribute__((__deprecated__))
#else
# define NS_GNUC_DEPRECATED
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
# define NS_GNUC_UNUSED __attribute__((__unused__))
# define NS_GNUC_NORETURN __attribute__((__noreturn__))
# define NS_GNUC_PRINTF(m, n) __attribute__((__format__ (__printf__, m, n))) NS_GNUC_NONNULL(m)
# define NS_GNUC_SCANF(m, n) __attribute__((__format__ (__scanf__, m, n))) NS_GNUC_NONNULL(m)
#else
# define NS_GNUC_UNUSED
# define NS_GNUC_NORETURN
# define NS_GNUC_PRINTF(fmtarg, firstvararg)
# define NS_GNUC_SCANF(fmtarg, firstvararg)
#endif


#if __GNUC_PREREQ(2,96)
# define NS_GNUC_MALLOC __attribute__((__malloc__))
# define NS_GNUC_PURE __attribute__((__pure__))
# define NS_GNUC_CONST __attribute__((__const__))
#else
# define NS_GNUC_MALLOC
# define NS_GNUC_PURE
# define NS_GNUC_CONST
#endif

#else /* _WIN32 */

# define NS_GNUC_SENTINEL
# define NS_GNUC_NONNULL
# define NS_GNUC_WARN_UNUSED_RESULT
# define NS_GNUC_MAYALIAS
# define NS_GNUC_DEPRECATED
# define NS_GNUC_USED
# define NS_GNUC_FORMAT(m)
# define NS_GNUC_UNUSED
# define NS_GNUC_NORETURN
# define NS_GNUC_PRINTF(fmtarg, firstvararg)
# define NS_GNUC_SCANF(fmtarg, firstvararg)
# define NS_GNUC_MALLOC
# define NS_GNUC_PURE
# define NS_GNUC_CONST

#endif /* _WIN32 */

/*
 * Ensure static RCSID strings aren't optimised away.
 */

#define NS_RCSID(string) static const char *RCSID NS_GNUC_USED = string \
    ", compiled: " __DATE__ " " __TIME__


#endif /* NSCHECK_H */
