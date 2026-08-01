dnl ----------------------------------------------------------------------
dnl NS_CHECK_GNU_ATOMIC_INT_BUILTINS
dnl
dnl Check whether the C compiler provides GCC-compatible atomic exchange
dnl and store operations for unsigned int, and guarantees that operations
dnl on that type are always lock-free.
dnl
dnl Defines:
dnl     HAVE_GNU_ATOMIC_UINT32_BUILTINS
dnl ----------------------------------------------------------------------

AC_DEFUN([AX_CHECK_GNU_ATOMIC_INT_BUILTINS], [
    AC_REQUIRE([AC_PROG_CC])
    AC_LANG_PUSH([C])

    AC_CACHE_CHECK(
        [for lock-free GCC-style atomic operations on unsigned int],
        [ns_cv_have_gnu_atomic_int_builtins],
        [AC_LINK_IFELSE(
            [AC_LANG_SOURCE([[
#if !defined(__GCC_ATOMIC_INT_LOCK_FREE)
# error "__GCC_ATOMIC_INT_LOCK_FREE is not defined"
#elif __GCC_ATOMIC_INT_LOCK_FREE != 2
# error "atomic operations on unsigned int are not always lock-free"
#endif

#include <stdint.h>
static unsigned int ns_atomic_value;

int
main(void)
{
    uint32_t previous;

    __atomic_store_n(&ns_atomic_value, 0u, __ATOMIC_RELAXED);
    previous = __atomic_exchange_n(&ns_atomic_value,
                                   1u,
                                   __ATOMIC_RELAXED);

    return previous != 0u;
}
            ]])],
            [ns_cv_have_gnu_atomic_int_builtins=yes],
            [ns_cv_have_gnu_atomic_int_builtins=no])]
    )

    AS_IF(
        [test "x$ns_cv_have_gnu_atomic_int_builtins" = xyes],
        [AC_DEFINE(
            [HAVE_GNU_ATOMIC_UINT32_BUILTINS],
            [1],
            [Define when the compiler provides lock-free GCC-style atomic operations on unsigned int.]
        )]
    )

    AM_CONDITIONAL([HAVE_GNU_ATOMIC_UINT32_BUILTINS], [test "x$ns_cv_have_gnu_atomic_int_builtins" = xyes])

    AC_LANG_POP([C])
])
