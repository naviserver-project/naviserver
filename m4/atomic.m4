dnl ----------------------------------------------------------------------
dnl AX_CHECK_GNU_ATOMIC_INT_BUILTINS
dnl
dnl Check for GCC-compatible lock-free atomic operations on fixed-width
dnl integer backing types.
dnl
dnl Defines:
dnl     HAVE_GNU_ATOMIC_UINT32_BUILTINS
dnl     HAVE_GNU_ATOMIC_UINT64_BUILTINS
dnl ----------------------------------------------------------------------

AC_DEFUN([AX_CHECK_GNU_ATOMIC_INT_BUILTINS], [
    AC_REQUIRE([AC_PROG_CC])
    AC_LANG_PUSH([C])

    AC_CACHE_CHECK(
        [for lock-free GCC-style atomic operations on 32-bit unsigned int],
        [ns_cv_have_gnu_atomic_uint32_builtins],
        [AC_LINK_IFELSE(
            [AC_LANG_SOURCE([[
#include <limits.h>
#include <stdint.h>

#if !defined(UINT32_MAX)
# error "uint32_t is not available"
#endif

#if UINT_MAX != UINT32_MAX
# error "unsigned int is not exactly 32 bits"
#endif

#if !defined(__GCC_ATOMIC_INT_LOCK_FREE)
# error "__GCC_ATOMIC_INT_LOCK_FREE is not defined"
#elif __GCC_ATOMIC_INT_LOCK_FREE != 2
# error "atomic operations on unsigned int are not always lock-free"
#endif

typedef char ns_atomic_uint32_size_check[
    sizeof(unsigned int) == sizeof(uint32_t) ? 1 : -1
];

static unsigned int ns_atomic_value;

int
main(void)
{
    uint32_t previous;

    __atomic_store_n(&ns_atomic_value, 0u, __ATOMIC_RELAXED);
    previous = (uint32_t)__atomic_exchange_n(&ns_atomic_value,
                                              1u,
                                              __ATOMIC_RELAXED);

    return previous != 0u;
}
            ]])],
            [ns_cv_have_gnu_atomic_uint32_builtins=yes],
            [ns_cv_have_gnu_atomic_uint32_builtins=no])]
    )

    AS_IF(
        [test "x$ns_cv_have_gnu_atomic_uint32_builtins" = xyes],
        [AC_DEFINE(
            [HAVE_GNU_ATOMIC_UINT32_BUILTINS],
            [1],
            [Define when unsigned int is 32 bits and supports lock-free GCC-style atomic operations.]
        )]
    )

    AM_CONDITIONAL([HAVE_GNU_ATOMIC_UINT32_BUILTINS],
                   [test "x$ns_cv_have_gnu_atomic_uint32_builtins" = xyes])

    AC_CACHE_CHECK(
        [for lock-free GCC-style atomic operations on uint64_t],
        [ns_cv_have_gnu_uint64_builtins],
        [AC_LINK_IFELSE(
            [AC_LANG_SOURCE([[
#include <stdint.h>

#if !defined(UINT64_MAX)
# error "uint64_t is not available"
#endif

/* Reject targets which would require an out-of-line atomic library. */
_Static_assert(
    __atomic_always_lock_free(sizeof(uint64_t), 0),
    "atomic operations on uint64_t are not always lock-free"
);

static uint64_t ns_atomic_value;

int
main(void)
{
    uint64_t previous;

    __atomic_store_n(&ns_atomic_value, 0, __ATOMIC_RELAXED);
    previous = __atomic_fetch_add(&ns_atomic_value, 1, __ATOMIC_RELAXED);

    return previous != 0
        || __atomic_load_n(&ns_atomic_value, __ATOMIC_RELAXED) != 1;
}
            ]])],
            [ns_cv_have_gnu_uint64_builtins=yes],
            [ns_cv_have_gnu_uint64_builtins=no])]
    )

    AS_IF(
        [test "x$ns_cv_have_gnu_uint64_builtins" = xyes],
        [AC_DEFINE(
            [HAVE_GNU_ATOMIC_UINT64_BUILTINS],
            [1],
            [Define when uint64_t supports lock-free GCC-style atomic operations.]
        )]
    )

    AM_CONDITIONAL([HAVE_GNU_ATOMIC_UINT64_BUILTINS],
                   [test "x$ns_cv_have_gnu_uint64_builtins" = xyes])

    AC_LANG_POP([C])
])
