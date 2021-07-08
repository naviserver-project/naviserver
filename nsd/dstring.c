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
 * dstring.c --
 *
 *      Ns_DString routines.  Ns_DString's are now compatible
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
Ns_DStringVarAppend(Ns_DString *dsPtr, ...)
{
    register const char *s;
    va_list              ap;

    va_start(ap, dsPtr);
    for (s = va_arg(ap, char *); s != NULL; s = va_arg(ap, char *)) {
        Ns_DStringAppend(dsPtr, s);
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
 *      Ns_DString is left in an initialized state.
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
Ns_DStringExport(Ns_DString *dsPtr)
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
    Ns_DStringFree(dsPtr);

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
Ns_DStringAppendArg(Ns_DString *dsPtr, const char *bytes)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(bytes != NULL);

    return Ns_DStringNAppend(dsPtr, bytes, (int) strlen(bytes) + 1);
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
Ns_DStringPrintf(Ns_DString *dsPtr, const char *fmt, ...)
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
Ns_DStringVPrintf(Ns_DString *dsPtr, const char *fmt, va_list apSrc)
{
    char    *buf;
    int      origLength, newLength, result;
    size_t   bufLength;
    va_list  ap;

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
    Ns_DStringSetLength(dsPtr, newLength);

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
         newLength = dsPtr->spaceAvl + (result - (int)bufLength);
#endif
        Ns_DStringSetLength(dsPtr, newLength);

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
        Ns_DStringSetLength(dsPtr, origLength + result);
    } else {
        Ns_DStringSetLength(dsPtr, origLength);
    }

    return Ns_DStringValue(dsPtr);
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
Ns_DStringAppendArgv(Ns_DString *dsPtr)
{
    char *s, **argv;
    int   i, argc, len, size;

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
    size = len + ((int)sizeof(char *) * (argc + 1));
    Ns_DStringSetLength(dsPtr, size);

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

Ns_DString *
Ns_DStringPop(void)
{
    Ns_DString *dsPtr;

    dsPtr = ns_malloc(sizeof(Ns_DString));
    Ns_DStringInit(dsPtr);
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
Ns_DStringPush(Ns_DString *dsPtr)
{
    Ns_DStringFree(dsPtr);
    ns_free(dsPtr);
}

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
Ns_DStringAppendPrintable(Tcl_DString *dsPtr, bool indentMode, const char *buffer, size_t len)
{
    size_t i;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    for (i = 0; i < len; i++) {
        unsigned char c = UCHAR(buffer[i]);

        if (c == '\n' && indentMode) {
            Tcl_DStringAppend(dsPtr, "\n:    ", 6);
        } else if ((CHARTYPE(print, c) == 0) || (c > UCHAR(127))) {
            Ns_DStringPrintf(dsPtr, "\\x%.2x", (c & 0xffu));
        } else {
            Ns_DStringPrintf(dsPtr, "%c", c);
        }
    }

    return Ns_DStringValue(dsPtr);
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
        Ns_DStringNAppend(dsPtr, "-", 1);
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



/*
 *----------------------------------------------------------------------
 * Compatibility routines --
 *
 *  Wrappers for old Ns_DString functions.
 *
 * Results:
 *      See Tcl_DString routine.
 *
 * Side effects:
 *      See Tcl_DString routine.
 *
 *----------------------------------------------------------------------
 */

#undef Ns_DStringInit

NS_EXTERN void Ns_DStringInit(Ns_DString *dsPtr) NS_GNUC_DEPRECATED_FOR(Tcl_DStringInit);

void
Ns_DStringInit(Ns_DString *dsPtr)
{
    Tcl_DStringInit(dsPtr);
}

#undef Ns_DStringFree

NS_EXTERN void Ns_DStringFree(Ns_DString *dsPtr) NS_GNUC_DEPRECATED_FOR(Tcl_DStringFree);

void
Ns_DStringFree(Ns_DString *dsPtr)
{
    Tcl_DStringFree(dsPtr);
}

#undef Ns_DStringSetLength

NS_EXTERN void Ns_DStringSetLength(Ns_DString *dsPtr, int length) NS_GNUC_DEPRECATED_FOR(Tcl_DStringSetLength);

void
Ns_DStringSetLength(Ns_DString *dsPtr, int length)
{
    Tcl_DStringSetLength(dsPtr, length);
}

#undef Ns_DStringTrunc

NS_EXTERN void Ns_DStringTrunc(Ns_DString *dsPtr, int length) NS_GNUC_DEPRECATED_FOR(Tcl_DStringSetLength);

void
Ns_DStringTrunc(Ns_DString *dsPtr, int length)
{
    Tcl_DStringSetLength(dsPtr, length);
}

#undef Ns_DStringNAppend

NS_EXTERN char *Ns_DStringNAppend(Ns_DString *dsPtr, const char *bytes, int length) NS_GNUC_DEPRECATED_FOR(Tcl_DStringAppend);

char *
Ns_DStringNAppend(Ns_DString *dsPtr, const char *bytes, int length)
{
    return Tcl_DStringAppend(dsPtr, bytes, length);
}

#undef Ns_DStringAppend

NS_EXTERN char *Ns_DStringAppend(Ns_DString *dsPtr, const char *bytes) NS_GNUC_DEPRECATED_FOR(Tcl_DStringAppend);

char *
Ns_DStringAppend(Ns_DString *dsPtr, const char *bytes)
{
    return Tcl_DStringAppend(dsPtr, bytes, -1);
}

#undef Ns_DStringAppendElement

NS_EXTERN char *Ns_DStringAppendElement(Ns_DString *dsPtr, const char *bytes) NS_GNUC_DEPRECATED_FOR(Tcl_DStringAppendElement);

char *
Ns_DStringAppendElement(Ns_DString *dsPtr, const char *bytes)
{
    return Tcl_DStringAppendElement(dsPtr, bytes);
}

#undef Ns_DStringLength

NS_EXTERN int Ns_DStringLength(const Ns_DString *dsPtr) NS_GNUC_DEPRECATED_FOR(TclstringlDStringLength);

int
Ns_DStringLength(const Ns_DString *dsPtr)
{
    return dsPtr->length;
}

#undef Ns_DStringValue

NS_EXTERN char *Ns_DStringValue(const Ns_DString *dsPtr) NS_GNUC_DEPRECATED_FOR(Tcl_DStringValue);

char *
Ns_DStringValue(const Ns_DString *dsPtr)
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
