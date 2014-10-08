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
 *	Reentrant versions of common system utilities using per-thread
 *	data buffers.  See the corresponding manual page for details.
 */

#include "thread.h"

/*
 * The following structure maintains state for the
 * reentrant wrappers.
 */

typedef struct Tls {
    char	    	nabuf[16];
    char		asbuf[27];
    char	       *stbuf;
    char		ctbuf[27];
    struct tm   	gtbuf;
    struct tm   	ltbuf;
#ifndef _WIN32
    struct dirent       ent;
#endif
} Tls;

static Ns_Tls tls;
static Tls *GetTls(void);

/*
 *----------------------------------------------------------------------
 *
 * NsInitReentrant --
 *
 *	Initialize reentrant function handling.  Some of the
 *      retentrant functions use to per-thread buffers (thread local
 *      storage) for reentrant routines
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Allocating thread local storage id.
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
 *	Return thread local storage. If not allocated yet, allocate
 *	memory via calloc.
 *
 * Results:
 *	Pointer to thread local storage
 *
 * Side effects:
 *	Allocating potentially memory .
 *
 *----------------------------------------------------------------------
 */
static Tls *
GetTls(void)
{
    Tls *tlsPtr;

    tlsPtr = Ns_TlsGet(&tls);
    if (tlsPtr == NULL) {
	tlsPtr = ns_calloc(1, sizeof(Tls));
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

#ifdef _MSC_VER
char *
ns_inet_ntoa(struct in_addr addr)
{
    Tls *tlsPtr = GetTls();

    /*
     * InetNtop supports AF_INET and AF_INET6.
     */
    InetNtop(AF_INET, &addr, tlsPtr->nabuf, sizeof(tlsPtr->nabuf)); 

    return tlsPtr->nabuf;
}
#else 
char *
ns_inet_ntoa(struct in_addr addr)
{
    Tls *tlsPtr = GetTls();

#if defined(HAVE_INET_NTOP)
    inet_ntop(AF_INET, &addr, tlsPtr->nabuf, sizeof(tlsPtr->nabuf)); 
#else
    union {
    	unsigned long l;
    	unsigned char b[4];
    } u;
    
    u.l = (unsigned long) addr.s_addr;
    snprintf(tlsPtr->nabuf, sizeof(tlsPtr->nabuf), "%u.%u.%u.%u",
             u.b[0], u.b[1], u.b[2], u.b[3]);
#endif
    return tlsPtr->nabuf;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * ns_readdir 
 *
 *----------------------------------------------------------------------
 */
#ifdef _WIN32
struct dirent *
ns_readdir(DIR * dir)
{
    return readdir(dir);
}
#else
struct dirent *
ns_readdir(DIR * dir)
{
    struct dirent *ent;
    Tls *tlsPtr = GetTls();

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

    errNum = localtime_s(&tlsPtr->ltbuf, clock);
    if (errNum) {
	NsThreadFatal("ns_localtime","localtime_s", errNum);
    }
    return &tlsPtr->ltbuf;

#elif defined(_WIN32)
    return localtime(clock);
#else
    Tls *tlsPtr = GetTls();
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

    errNum = gmtime_s(&tlsPtr->gtbuf, clock);
    if (errNum) {
	NsThreadFatal("ns_gmtime","gmtime_s", errNum);
     }

    return &tlsPtr->gtbuf;

#elif defined(_WIN32)

    return gmtime(clock);

#else

    Tls *tlsPtr = GetTls();
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

    errNum = ctime_s(tlsPtr->ctbuf, sizeof(tlsPtr->ctbuf), clock);
    if (errNum) {
	NsThreadFatal("ns_ctime","ctime_s", errNum);
    }

    return tlsPtr->ctbuf;

#elif defined(_WIN32)

    return ctime(clock);

#else
    Tls *tlsPtr = GetTls();
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

    errNum = asctime_s(tlsPtr->asbuf, sizeof(tlsPtr->asbuf), tmPtr);
    if (errNum) {
	NsThreadFatal("ns_asctime","asctime_s", errNum);
    }

    return tlsPtr->asbuf;

#elif defined(_WIN32)

    return asctime(tmPtr);

#else

    Tls *tlsPtr = GetTls();
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
ns_strtok(char *s1, const char *s2)
{
#ifdef _MSC_VER

    Tls *tlsPtr = GetTls();
    return strtok_s(s1, s2, &tlsPtr->stbuf);

#elif defined(_WIN32)

    return strtok(s1, s2);

#else

    Tls *tlsPtr = GetTls();
    return strtok_r(s1, s2, &tlsPtr->stbuf);

#endif
}




