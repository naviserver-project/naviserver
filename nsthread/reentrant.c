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
 * reentrant.c --
 *
 *    Reentrant versions of common system utilities using per-thread
 *    data buffers.  See the corresponding manual page for details.
 */

#include "thread.h"

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
ns_inet_ntoa(struct sockaddr *saPtr)
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
        sprintf(tlsPtr->nabuf, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                ntohs(S6_ADDR16(addr)[0]), ntohs(S6_ADDR16(addr)[1]),
                ntohs(S6_ADDR16(addr)[2]), ntohs(S6_ADDR16(addr)[3]),
                ntohs(S6_ADDR16(addr)[4]), ntohs(S6_ADDR16(addr)[5]),
                ntohs(S6_ADDR16(addr)[6]), ntohs(S6_ADDR16(addr)[7]));
# else
        sprintf(tlsPtr->nabuf, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                ntohs(addr.u.Word[0]), ntohs(addr.u.Word[1]),
                ntohs(addr.u.Word[2]), ntohs(addr.u.Word[3]),
                ntohs(addr.u.Word[4]), ntohs(addr.u.Word[5]),
                ntohs(addr.u.Word[6]), ntohs(addr.u.Word[7]));
# endif
    } else {
        addr4.i = (unsigned int) (((struct sockaddr_in *)saPtr)->sin_addr.s_addr);
        sprintf(tlsPtr->nabuf, "%u.%u.%u.%u", addr4.b[0], addr4.b[1], addr4.b[2], addr4.b[3]);
    }
#else
    addr4.i = (unsigned int) (((struct sockaddr_in *)saPtr)->sin_addr.s_addr);
    sprintf(tlsPtr->nabuf, "%u.%u.%u.%u", addr4.b[0], addr4.b[1], addr4.b[2], addr4.b[3]);
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
ns_readdir(DIR * dir)
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
ns_localtime(const time_t *clock)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();
    int errNum;

    NS_NONNULL_ASSERT(clock != NULL);

    errNum = localtime_s(&tlsPtr->ltbuf, clock);
    if (errNum != 0) {
        NsThreadFatal("ns_localtime", "localtime_s", errNum);
    }

    return &tlsPtr->ltbuf;

#elif defined(_WIN32)
    NS_NONNULL_ASSERT(clock != NULL);
    return localtime(clock);
#else
    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(clock != NULL);
    return localtime_r(clock, &tlsPtr->ltbuf);

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
ns_gmtime(const time_t *clock)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();
    int errNum;

    NS_NONNULL_ASSERT(clock != NULL);
    errNum = gmtime_s(&tlsPtr->gtbuf, clock);
    if (errNum != 0) {
        NsThreadFatal("ns_gmtime", "gmtime_s", errNum);
    }

    return &tlsPtr->gtbuf;

#elif defined(_WIN32)

    NS_NONNULL_ASSERT(clock != NULL);
    return gmtime(clock);

#else

    Tls *tlsPtr = GetTls();
    NS_NONNULL_ASSERT(clock != NULL);
    return gmtime_r(clock, &tlsPtr->gtbuf);

#endif
}


/*
 *----------------------------------------------------------------------
 *
 * ns_ctime
 *
 *----------------------------------------------------------------------
 */
char *
ns_ctime(const time_t *clock)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();
    int errNum;

    NS_NONNULL_ASSERT(clock != NULL);
    errNum = ctime_s(tlsPtr->ctbuf, sizeof(tlsPtr->ctbuf), clock);
    if (errNum != 0) {
        NsThreadFatal("ns_ctime", "ctime_s", errNum);
    }

    return tlsPtr->ctbuf;

#elif defined(_WIN32)

    NS_NONNULL_ASSERT(clock != NULL);
    return ctime(clock);

#else
    Tls *tlsPtr = GetTls();
    NS_NONNULL_ASSERT(clock != NULL);
    return ctime_r(clock, tlsPtr->ctbuf);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * ns_asctime
 *
 *----------------------------------------------------------------------
 */

char *
ns_asctime(const struct tm *tmPtr)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();
    int errNum;

    NS_NONNULL_ASSERT(tmPtr != NULL);

    errNum = asctime_s(tlsPtr->asbuf, sizeof(tlsPtr->asbuf), tmPtr);
    if (errNum != 0) {
        NsThreadFatal("ns_asctime", "asctime_s", errNum);
    }

    return tlsPtr->asbuf;

#elif defined(_WIN32)
    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(tmPtr != NULL);

    (void)strftime(tlsPtr->asbuf, sizeof(tlsPtr->asbuf), "%a %b %e %T %Y", tmPtr);
    return tlsPtr->asbuf;

#else
    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(tmPtr != NULL);

    return asctime_r(tmPtr, tlsPtr->asbuf);

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

    NS_NONNULL_ASSERT(str != NULL);
    NS_NONNULL_ASSERT(sep != NULL);

    return strtok_s(str, sep, &tlsPtr->stbuf);

#elif defined(_WIN32)

    NS_NONNULL_ASSERT(str != NULL);
    NS_NONNULL_ASSERT(sep != NULL);
    return strtok(str, sep);

#else

    Tls *tlsPtr = GetTls();

    NS_NONNULL_ASSERT(str != NULL);
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
