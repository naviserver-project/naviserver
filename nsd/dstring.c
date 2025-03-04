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
 * dstring.c --
 *
 *      Tcl_DString routines.  Ns_DString's are now compatible
 *      with Tcl_DString's.
 */

#include "nsd.h"


/*
 *----------------------------------------------------------------------
 *
 * Ns_DStringVarAppend --
 *
 *      Append a variable number of string arguments to a dstring.
 *
 * Results:
 *      Pointer to current dstring value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_DStringVarAppend(Tcl_DString *dsPtr, ...)
{
    register const char *s;
    va_list              ap;

    va_start(ap, dsPtr);
    for (s = va_arg(ap, char *); s != NULL; s = va_arg(ap, char *)) {
        Tcl_DStringAppend(dsPtr, s, TCL_INDEX_NONE);
    }
    va_end(ap);

    return dsPtr->string;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DStringExport --
 *
 *      Return a copy of the string value on the heap.
 *      Tcl_DString is left in an initialized state.
 *
 * Results:
 *      Pointer to ns_malloc'ed string which must be eventually freed.
 *
 * Side effects:
 *      None.
 *
 *
 *----------------------------------------------------------------------
 */

char *
Ns_DStringExport(Tcl_DString *dsPtr)
{
    char   *s;
    size_t  size;

    NS_NONNULL_ASSERT(dsPtr != NULL);

#ifdef SAME_MALLOC_IN_TCL_AND_NS
    /*
     * The following code saves a memory duplication in case the Tcl_DString
     * was allocated with the same memory allocator from Tcl as used for
     * freeing in NaviServer. This is the case, when both Tcl (+ SYSTEM_MALLOC
     * patch) and NaviServer were compiled with SYSTEM_MALLOC, or both without
     * it. The save assumption is that we cannot trust on this and we do not
     * define this flag.
     */
    if (dsPtr->string != dsPtr->staticSpace) {
        s = dsPtr->string;
        dsPtr->string = dsPtr->staticSpace;
    } else {
        size = (size_t)dsPtr->length + 1u;
        s = ns_malloc(size);
        memcpy(s, dsPtr->string, size);
    }
#else
    size = (size_t)dsPtr->length + 1u;
    s = ns_malloc(size);
    if (likely(s != NULL)) {
        memcpy(s, dsPtr->string, size);
    }
#endif
    Tcl_DStringFree(dsPtr);

    return s;
}


/*
 *----------------------------------------------------------------------
 * Ns_DStringAppendArg --
 *
 *      Append a string including its terminating null byte.
 *
 * Results:
 *      Pointer to the current string value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_DStringAppendArg(Tcl_DString *dsPtr, const char *bytes)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(bytes != NULL);

    return Tcl_DStringAppend(dsPtr, bytes, (TCL_SIZE_T)strlen(bytes) + 1);
}


/*
 *----------------------------------------------------------------------
 * Ns_DStringPrintf --
 *
 *      Append a sequence of values using a format string.
 *
 * Results:
 *      Pointer to the current string value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_DStringPrintf(Tcl_DString *dsPtr, const char *fmt, ...)
{
    char           *str;
    va_list         ap;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    va_start(ap, fmt);
    str = Ns_DStringVPrintf(dsPtr, fmt, ap);
    va_end(ap);

    return str;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DStringVPrintf --
 *
 *      Append a sequence of values using a format string.
 *
 * Results:
 *      Pointer to the current string value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_DStringVPrintf(Tcl_DString *dsPtr, const char *fmt, va_list apSrc)
{
    char      *buf;
    int        result;
    TCL_SIZE_T origLength, newLength;
    size_t     bufLength;
    va_list    ap;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(fmt != NULL);

    origLength = dsPtr->length;

    /*
     * Extend the dstring, trying first to fit everything in the
     * static space (unless it is unreasonably small), or if
     * we already have an allocated buffer just bump it up by 1k.
     */

    if (dsPtr->spaceAvl < TCL_INTEGER_SPACE) {
        newLength = dsPtr->length + 1024;
    } else {
        newLength = dsPtr->spaceAvl -1; /* leave space for dstring NIL */
    }
    Tcl_DStringSetLength(dsPtr, newLength);

    /*
     * Now that any dstring buffer relocation has taken place it is
     * safe to point into the middle of it at the end of the
     * existing data.
     */

    buf = dsPtr->string + origLength;
    bufLength = (size_t)newLength - (size_t)origLength;

    va_copy(ap, apSrc);
    result = vsnprintf(buf, bufLength, fmt, ap);
    va_end(ap);

    /*
     * Check for overflow and retry. For win32 just double the buffer size
     * and iterate, otherwise we should get this correct first time.
     */
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER < 1900)
    while (result == -1 && errno == ERANGE) {
        newLength = dsPtr->spaceAvl * 2;
#else
    if ((size_t)result >= bufLength) {
        newLength = dsPtr->spaceAvl + ((TCL_SIZE_T)result - (TCL_SIZE_T)bufLength);
#endif
        Tcl_DStringSetLength(dsPtr, newLength);

        buf = dsPtr->string + origLength;
        bufLength = (size_t)newLength - (size_t)origLength;

        va_copy(ap, apSrc);
        result = vsnprintf(buf, bufLength, fmt, ap);
        va_end(ap);
    }

    /*
     * Set the dstring buffer to the actual length.
     * NB: Eat any errors.
     */

    if (result > 0) {
        Tcl_DStringSetLength(dsPtr, origLength + (TCL_SIZE_T)result);
    } else {
        Tcl_DStringSetLength(dsPtr, origLength);
    }

    return dsPtr->string;
}


/*
 *----------------------------------------------------------------------
 * Ns_DStringAppendArgv --
 *
 *      Append an argv vector pointing to the null terminated
 *      strings in the given dstring.
 *
 * Results:
 *      Pointer char ** vector appended to end of dstring.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char **
Ns_DStringAppendArgv(Tcl_DString *dsPtr)
{
    char      *s, **argv;
    TCL_SIZE_T len, size, i, argc;

    /*
     * Determine the number of strings.
     */

    NS_NONNULL_ASSERT(dsPtr != NULL);

    argc = 0;
    s = dsPtr->string;
    while (*s != '\0') {
        ++argc;
        s += strlen(s) + 1u;
    }

    /*
     * Resize the dstring with space for the argv aligned
     * on an 8 byte boundary.
     */

    len = ((dsPtr->length / 8) + 1) * 8;
    size = len + ((TCL_SIZE_T)sizeof(char *) * (argc + 1));
    Tcl_DStringSetLength(dsPtr, size);

    /*
     * Set the argv elements to the strings.
     */

    s = dsPtr->string;
    argv = (char **) (s + len);
    for (i = 0; i < argc; ++i) {
        argv[i] = s;
        s += strlen(s) + 1u;
    }
    argv[i] = NULL;

    return argv;
}

#ifdef NS_WITH_DEPRECATED
/*
 *----------------------------------------------------------------------
 * Ns_DStringPop --
 *
 *      Allocate a new dstring.
 *      Deprecated.
 *
 * Results:
 *      Pointer to Ns_DString.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DString *
Ns_DStringPop(void)
{
    Tcl_DString *dsPtr;

    dsPtr = ns_malloc(sizeof(Ns_DString));
    Tcl_DStringInit(dsPtr);
    return dsPtr;
}

/*
 *----------------------------------------------------------------------
 * Ns_DStringPush --
 *
 *      Free a dstring.
 *      Deprecated.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DStringPush(Tcl_DString *dsPtr)
{
    Tcl_DStringFree(dsPtr);
    ns_free(dsPtr);
}
#endif

/*----------------------------------------------------------------------
 *
 * Ns_DStringAppendPrintable --
 *
 *      Append buffer containing potentially non-printable characters in
 *      printable way to an already initialized DString. The function appends
 *      printable characters and space as is, and appends otherwise the hex
 *      code if the bytes with a \x prefix.
 *
 * Results:
 *      DString value.
 *
 * Side effects:
 *      Appends to the DString
 *
 *----------------------------------------------------------------------
 */
char *
Ns_DStringAppendPrintable(Tcl_DString *dsPtr, bool indentMode, bool tabExpandMode, const char *buffer, size_t len)
{
    size_t i;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    for (i = 0; i < len; i++) {
        unsigned char c = UCHAR(buffer[i]);

        if (c == '\n' && indentMode) {
            Tcl_DStringAppend(dsPtr, "\n:    ", 6);
        } else if (c == '\t' && tabExpandMode) {
            Tcl_DStringAppend(dsPtr, "    ", 4);
        } else if ((CHARTYPE(print, c) == 0) || (c > UCHAR(127))) {
            Ns_DStringPrintf(dsPtr, "\\x%.2x", (c & 0xffu));
        } else {
            Ns_DStringPrintf(dsPtr, "%c", c);
        }
    }

    return dsPtr->string;
}

/*----------------------------------------------------------------------
 *
 * Ns_DStringAppendTime --
 *
 *      Append the given time to DString formatted in a uniform way.
 *
 * Results:
 *      DString value
 *
 * Side effects:
 *      Appends to the DString
 *
 *----------------------------------------------------------------------
 */
char *
Ns_DStringAppendTime(Tcl_DString *dsPtr, const Ns_Time *timePtr)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(timePtr != NULL);

    if (timePtr->sec < 0 || (timePtr->sec == 0 && timePtr->usec < 0)) {
        Tcl_DStringAppend(dsPtr, "-", 1);
    }
    if (timePtr->usec == 0) {
        Ns_DStringPrintf(dsPtr, "%lld", llabs(timePtr->sec));
    } else {
        Ns_DStringPrintf(dsPtr, "%lld.%06ld",
                         llabs(timePtr->sec), labs(timePtr->usec));
        /*
         * Strip trailing zeros after comma dot.
         */
        while (dsPtr->string[dsPtr->length-1] == '0') {
            dsPtr->length --;
        }
    }
    return dsPtr->string;
}

/*----------------------------------------------------------------------
 *
 * Ns_DStringAppendSockState --
 *
 *      Append the provided Ns_SockState in human readable form
 *
 * Results:
 *      DString value
 *
 * Side effects:
 *      Appends to the DString
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_DStringAppendSockState(Tcl_DString *dsPtr, Ns_SockState state)
{
    int    count = 0;
    size_t i;
    static const struct {
        Ns_SockState state;
        const char  *label;
    } options[] = {
        { NS_SOCK_NONE,      "NONE"},
        { NS_SOCK_READ,      "READ"},
        { NS_SOCK_WRITE,     "WRITE"},
        { NS_SOCK_EXCEPTION, "EXCEPTION"},
        { NS_SOCK_EXIT,      "EXIT"},
        { NS_SOCK_DONE,      "DONE"},
        { NS_SOCK_CANCEL,    "CANCEL"},
        { NS_SOCK_TIMEOUT,   "TIMEOUT"},
        { NS_SOCK_AGAIN,     "AGAIN"},
        { NS_SOCK_INIT,      "INIT"}
    };

    NS_NONNULL_ASSERT(dsPtr != NULL);

    for (i = 0; i<sizeof(options)/sizeof(options[0]); i++) {
        if ((options[i].state & state) != 0u) {
            if (count > 0) {
                Tcl_DStringAppend(dsPtr, "|", 1);
            }
            Tcl_DStringAppend(dsPtr, options[i].label, TCL_INDEX_NONE);
            count ++;
        }
    }
    return dsPtr->string;
}


/*
 *----------------------------------------------------------------------
 * Compatibility routines --
 *
 *  Wrappers for old Tcl_DString functions.
 *
 * Results:
 *      See Tcl_DString routine.
 *
 * Side effects:
 *      See Tcl_DString routine.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DStringInit(Tcl_DString *dsPtr)
{
    Tcl_DStringInit(dsPtr);
}

void
Ns_DStringFree(Tcl_DString *dsPtr)
{
    Tcl_DStringFree(dsPtr);
}

void
Ns_DStringSetLength(Tcl_DString *dsPtr, TCL_SIZE_T length)
{
    Tcl_DStringSetLength(dsPtr, length);
}

void
Ns_DStringTrunc(Tcl_DString *dsPtr, TCL_SIZE_T length)
{
    Tcl_DStringSetLength(dsPtr, length);
}

char *
Ns_DStringNAppend(Tcl_DString *dsPtr, const char *bytes, TCL_SIZE_T length)
{
    return Tcl_DStringAppend(dsPtr, bytes, length);
}

char *
Ns_DStringAppend(Tcl_DString *dsPtr, const char *bytes)
{
    return Tcl_DStringAppend(dsPtr, bytes, TCL_INDEX_NONE);
}

char *
Ns_DStringAppendElement(Tcl_DString *dsPtr, const char *bytes)
{
    return Tcl_DStringAppendElement(dsPtr, bytes);
}

TCL_SIZE_T
Ns_DStringLength(const Tcl_DString *dsPtr)
{
    return dsPtr->length;
}

char *
Ns_DStringValue(const Tcl_DString *dsPtr)
{
    return dsPtr->string;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
