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
 * reentrant.c --
 *
 *    Reentrant versions of common system utilities using per-thread
 *    data buffers.  See the corresponding manual page for details.
 */

#include "thread.h"

#ifdef _GNU_SOURCE
//#yes
#endif

/*
 * The following structure maintains state for the
 * reentrant wrappers.
 */

typedef struct Tls {
    char        nabuf[NS_IPADDR_SIZE];
    char        asbuf[27];
    char        ctbuf[27];
    char       *stbuf;
    struct tm   gtbuf;
    struct tm   ltbuf;
#ifndef _WIN32
    struct dirent ent;
#endif
} Tls;

static Ns_Tls tls;
static Tls *GetTls(void) NS_GNUC_RETURNS_NONNULL;

/*
 *----------------------------------------------------------------------
 *
 * NsInitReentrant --
 *
 *    Initialize reentrant function handling.  Some of the
 *      retentrant functions use to per-thread buffers (thread local
 *      storage) for reentrant routines
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Allocating thread local storage id.
 *
 *----------------------------------------------------------------------
 */
void
NsInitReentrant(void)
{
    Ns_TlsAlloc(&tls, ns_free);
}

/*
 *----------------------------------------------------------------------
 *
 * GetTls --
 *
 *    Return thread local storage. If not allocated yet, allocate
 *    memory via calloc.
 *
 * Results:
 *    Pointer to thread local storage
 *
 * Side effects:
 *    Allocating potentially memory .
 *
 *----------------------------------------------------------------------
 */
static Tls *
GetTls(void)
{
    Tls *tlsPtr;

    tlsPtr = Ns_TlsGet(&tls);
    if (tlsPtr == NULL) {
        tlsPtr = ns_calloc(1u, sizeof(Tls));
        Ns_TlsSet(&tls, tlsPtr);
    }
    return tlsPtr;
}



/*
 *----------------------------------------------------------------------
 *
 * ns_inet_ntoa
 *
 *----------------------------------------------------------------------
 */

char *
ns_inet_ntoa(const struct sockaddr *saPtr)
{
    Tls *tlsPtr = GetTls();
    union {
        unsigned int i;
        unsigned char b[4];
    } addr4;

    NS_NONNULL_ASSERT(saPtr != NULL);

#ifdef HAVE_IPV6
    if (saPtr->sa_family == AF_INET6) {
        struct in6_addr addr = (((struct sockaddr_in6 *)saPtr)->sin6_addr);
# ifndef _WIN32
        snprintf(tlsPtr->nabuf, NS_IPADDR_SIZE, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                ntohs(S6_ADDR16(addr)[0]), ntohs(S6_ADDR16(addr)[1]),
                ntohs(S6_ADDR16(addr)[2]), ntohs(S6_ADDR16(addr)[3]),
                ntohs(S6_ADDR16(addr)[4]), ntohs(S6_ADDR16(addr)[5]),
                ntohs(S6_ADDR16(addr)[6]), ntohs(S6_ADDR16(addr)[7]));
# else
        snprintf(tlsPtr->nabuf, NS_IPADDR_SIZE, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                ntohs(addr.u.Word[0]), ntohs(addr.u.Word[1]),
                ntohs(addr.u.Word[2]), ntohs(addr.u.Word[3]),
                ntohs(addr.u.Word[4]), ntohs(addr.u.Word[5]),
                ntohs(addr.u.Word[6]), ntohs(addr.u.Word[7]));
# endif
    } else {
        addr4.i = (unsigned int) (((struct sockaddr_in *)saPtr)->sin_addr.s_addr);
        snprintf(tlsPtr->nabuf, NS_IPADDR_SIZE, "%u.%u.%u.%u", addr4.b[0], addr4.b[1], addr4.b[2], addr4.b[3]);
    }
#else
    addr4.i = (unsigned int) (((struct sockaddr_in *)saPtr)->sin_addr.s_addr);
    snprintf(tlsPtr->nabuf, NS_IPADDR_SIZE, "%u.%u.%u.%u", addr4.b[0], addr4.b[1], addr4.b[2], addr4.b[3]);
#endif

    return tlsPtr->nabuf;
}

/*
 *----------------------------------------------------------------------
 *
 * ns_readdir
 *
 *----------------------------------------------------------------------
 */
#ifndef USE_READDIR_R
struct dirent *
ns_readdir(DIR *dir)
{
    NS_NONNULL_ASSERT(dir != NULL);
    return readdir(dir);
}
#else
struct dirent *
ns_readdir(DIR *dir)
{
    struct dirent *ent;
    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(dir != NULL);

    ent = &tlsPtr->ent;
    if (readdir_r(dir, ent, &ent) != 0) {
        ent = NULL;
    }
    return ent;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * ns_localtime
 *
 *----------------------------------------------------------------------
 */
struct tm *
ns_localtime(const time_t *timep)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();
    int errNum;

    NS_NONNULL_ASSERT(timep != NULL);

    errNum = localtime_s(&tlsPtr->ltbuf, timep);
    if (errNum != 0) {
        NsThreadFatal("ns_localtime", "localtime_s", errNum);
    }

    return &tlsPtr->ltbuf;

#elif defined(_WIN32)
    NS_NONNULL_ASSERT(timep != NULL);
    return localtime(timep);
#else
    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(timep != NULL);

    return localtime_r(timep, &tlsPtr->ltbuf);
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * ns_localtime_r
 *
 *     Same as ns_localtime(), except that the function uses user-provided
 *     storage buf for the result.
 *
 *----------------------------------------------------------------------
 */
struct tm *
ns_localtime_r(const time_t *timer, struct tm *buf)
{
#ifdef _MSC_VER
    /*
     * Microsoft C (Visual Studio)
     */
    int errNum;

    NS_NONNULL_ASSERT(timer != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    errNum = localtime_s(buf, timer);
    if (errNum != 0) {
        NsThreadFatal("ns_localtime_r", "localtime_s", errNum);
    }

    return buf;

#elif defined(_WIN32)
    /*
     * Other win compiler.
     */
    NS_NONNULL_ASSERT(timer != NULL);
    *buf = *localtime(timer);
    return buf;
#else
    NS_NONNULL_ASSERT(timer != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    return localtime_r(timer, buf);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * ns_gmtime
 *
 *----------------------------------------------------------------------
 */
struct tm *
ns_gmtime(const time_t *timep)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();
    int errNum;

    NS_NONNULL_ASSERT(timep != NULL);
    errNum = gmtime_s(&tlsPtr->gtbuf, timep);
    if (errNum != 0) {
        NsThreadFatal("ns_gmtime", "gmtime_s", errNum);
    }

    return &tlsPtr->gtbuf;

#elif defined(_WIN32)

    NS_NONNULL_ASSERT(timep != NULL);
    return gmtime(timep);

#else

    Tls *tlsPtr = GetTls();
    NS_NONNULL_ASSERT(timep != NULL);
    return gmtime_r(timep, &tlsPtr->gtbuf);

#endif
}

/*
 *----------------------------------------------------------------------
 *
 * ns_strtok
 *
 *----------------------------------------------------------------------
 */

char *
ns_strtok(char *str, const char *sep)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(sep != NULL);

    return strtok_s(str, sep, &tlsPtr->stbuf);

#elif defined(_WIN32)

    NS_NONNULL_ASSERT(sep != NULL);
    return strtok(str, sep);

#else

    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(sep != NULL);
    return strtok_r(str, sep, &tlsPtr->stbuf);

#endif
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
