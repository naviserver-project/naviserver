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
 * dns.c --
 *
 *      DNS lookup routines.
 */

#include "nsd.h"

/*
 * W2000 has no getaddrinfo, requires special headers for inline functions
 */

#ifdef _MSC_VER
#  include <Ws2tcpip.h>
/*#  include <Wspiapi.h>*/
#endif

#ifndef INADDR_NONE
#define INADDR_NONE (-1)
#endif

#ifndef NETDB_INTERNAL
#  ifdef h_NETDB_INTERNAL
#  define NETDB_INTERNAL h_NETDB_INTERNAL
#  endif
#endif

#ifdef NEED_HERRNO
extern int h_errno;
#endif


typedef bool (GetProc)(Ns_DString *dsPtr, const char *key);


/*
 * Static functions defined in this file
 */

static GetProc GetAddr;
static GetProc GetHost;
static bool DnsGet(GetProc *getProc, Ns_DString *dsPtr,
                   Ns_Cache *cache, const char *key, int all)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);


#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETNAMEINFO)
static void LogError(char *func, int h_errnop);
#endif


/*
 * Static variables defined in this file
 */

static Ns_Cache *hostCache;
static Ns_Cache *addrCache;
static int       ttl;       /* Time in senconds each entry can live in the cache. */
static int       timeout;   /* Time in seconds to wait for concurrent update.  */



/*
 *----------------------------------------------------------------------
 *
 * NsConfigDNS --
 *
 *      Enable DNS results caching.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Futher DNS lookups will be cached using given ttl.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigDNS(void)
{
    const char *path = NS_CONFIG_PARAMETERS;

    if (Ns_ConfigBool(path, "dnscache", NS_TRUE) == NS_TRUE) {
        int max = Ns_ConfigIntRange(path, "dnscachemaxsize", 1024*500, 0, INT_MAX);
        
        if (max > 0) {
            timeout = Ns_ConfigIntRange(path, "dnswaittimeout",  5, 0, INT_MAX);
            ttl = Ns_ConfigIntRange(path, "dnscachetimeout", 60, 0, INT_MAX);
            ttl *= 60; /* NB: Config specifies minutes, seconds used internally. */

            hostCache = Ns_CacheCreateSz("ns:dnshost", TCL_STRING_KEYS,
                                         (size_t) max, ns_free);
            addrCache = Ns_CacheCreateSz("ns:dnsaddr", TCL_STRING_KEYS,
                                         (size_t) max, ns_free);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetHostByAddr, Ns_GetAddrByHost --
 *
 *      Convert an IP address to a hostname or vice versa.
 *
 * Results:
 *      NS_TRUE and result is appended to dstring, NS_FALSE if name or
 *      address not found. An error message may be logged if not found.
 *
 * Side effects:
 *      Result may be cached.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_GetHostByAddr(Ns_DString *dsPtr, const char *addr)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(addr != NULL);
    
    return DnsGet(GetHost, dsPtr, hostCache, addr, 0);
}

bool
Ns_GetAddrByHost(Ns_DString *dsPtr, const char *host)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(host != NULL);

    return DnsGet(GetAddr, dsPtr, addrCache, host, 0);
}


bool
Ns_GetAllAddrByHost(Ns_DString *dsPtr, const char *host)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(host != NULL);
    
    return DnsGet(GetAddr, dsPtr, addrCache, host, 1);
}

static bool
DnsGet(GetProc *getProc, Ns_DString *dsPtr, Ns_Cache *cache, const char *key, int all)
{
    Ns_DString  ds;
    Ns_Time     t;
    int         isNew;
    bool        success;

    NS_NONNULL_ASSERT(getProc != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);
        
    /*
     * Call getProc directly or through cache.
     */

    Ns_DStringInit(&ds);
    if (cache == NULL) {
        success = (*getProc)(&ds, key);
    } else {
        Ns_Entry   *entry;

        Ns_GetTime(&t);
        Ns_IncrTime(&t, timeout, 0);

        Ns_CacheLock(cache);
        entry = Ns_CacheWaitCreateEntry(cache, key, &isNew, &t);
        if (entry == NULL) {
            Ns_CacheUnlock(cache);
            Ns_Log(Notice, "dns: timeout waiting for concurrent update");
            return NS_FALSE;
        }
        if (isNew != 0) {
            Ns_CacheUnlock(cache);
            success = (*getProc)(&ds, key);
            Ns_CacheLock(cache);
            if (!success) {
                Ns_CacheDeleteEntry(entry);
            } else {
	        Ns_Time endTime, diffTime;

                Ns_GetTime(&endTime);
		(void)Ns_DiffTime(&endTime, &t, &diffTime);
                Ns_IncrTime(&endTime, ttl, 0);
                Ns_CacheSetValueExpires(entry, ns_strdup(ds.string), 
					(size_t)ds.length, &endTime, 
					(int)(diffTime.sec * 1000000 + diffTime.usec));
            }
            Ns_CacheBroadcast(cache);
        } else {
            Ns_DStringNAppend(&ds, Ns_CacheGetValue(entry),
                              (int)Ns_CacheGetSize(entry));
            success = NS_TRUE;
        }
        Ns_CacheUnlock(cache);
    }

    if (success) {
        if (getProc == GetAddr && all == 0) {
            const char *p = ds.string;
            while (*p != '\0' && CHARTYPE(space, *p) == 0) {
                ++p;
            }
            Ns_DStringSetLength(&ds, (int)(p - ds.string));
        }
        Ns_DStringNAppend(dsPtr, ds.string, ds.length);
    }
    Ns_DStringFree(&ds);

    return success;
}


/**********************************************************************
 * Begin IPv6
 **********************************************************************/
#ifdef HAVE_IPV6

/*
 *----------------------------------------------------------------------
 * GetHost, GetAddr --
 *
 *      Perform the actual lookup by host or address.
 *
 * Results:
 *      If a name can be found, the function returns NS_TRUE; otherwise,
 *      it returns NS_FALSE.
 *
 * Side effects:
 *      Result is appended to dsPtr.
 *
 *----------------------------------------------------------------------
 */

static bool
GetHost(Ns_DString *dsPtr, const char *addr)
{
    int    r;
    struct sockaddr_storage sa;
    struct sockaddr        *saPtr = (struct sockaddr *)&sa;
    bool   result = NS_FALSE;

    r = ns_inet_pton(saPtr, addr);
    if (r > 0) {
        char buf[NI_MAXHOST];
        int  err;

        err = getnameinfo(saPtr,
                          (sa.ss_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in),
                          buf, sizeof(buf),
                          NULL, 0, NI_NAMEREQD);
        if (err != 0) {
            Ns_Log(Notice, "dns: getnameinfo failed for addr <%s>: %s", addr, gai_strerror(err));
        } else {
            Ns_DStringAppend(dsPtr, buf);
            result = NS_TRUE;
        }
    }

    return result;
}

static bool
GetAddr(Ns_DString *dsPtr, const char *host)
{
    struct addrinfo hints;
    const struct addrinfo *ptr;
    struct addrinfo       *res;
    int result;
    bool status = NS_FALSE;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_STREAM;

    result = getaddrinfo(host, NULL, &hints, &res);
    if (result == 0) {
        ptr = res;
        while (ptr != NULL) {
            char ipString[NS_IPADDR_SIZE];

            /*
             * Getaddrinfo with flag AF_UNSPEC returns both AF_INET and
             * AF_INET6 addresses.
             */
            /*fprintf(stderr, "##### getaddrinfo <%s> -> %d family %s\n",
                    host,
                    ptr->ai_family,
                    (ptr->ai_family == AF_INET6) ? "AF_INET6" : "AF_INET");*/
            if ((ptr->ai_family != AF_INET) && (ptr->ai_family != AF_INET6)) {
                Ns_Log(Error, "dns: getaddrinfo failed for %s: unknown address family %d",
                       host, ptr->ai_family);
                freeaddrinfo(res);
                return NS_FALSE;
            }
            Tcl_DStringAppendElement(dsPtr,
                                     ns_inet_ntop(ptr->ai_addr, ipString, sizeof(ipString)));
                
            status = NS_TRUE;
            ptr = ptr->ai_next;
        }
        freeaddrinfo(res);

    } else if (result != EAI_NONAME) {
        Ns_Log(Error, "dns: getaddrinfo failed for %s: %s", host,
               gai_strerror(result));
    }
    
    return status;
}
    
#else
/**********************************************************************
 * Begin no IPv6
 **********************************************************************/

/*
 *----------------------------------------------------------------------
 * GetHost, GetAddr --
 *
 *      Perform the actual lookup by host or address.
 *
 *      NOTE: A critical section is used instead of a mutex
 *      to ensure waiting on a condition and not mutex spin waiting.
 *
 * Results:
 *      If a name can be found, the function returns NS_TRUE; otherwise,
 *      it returns NS_FALSE.
 *
 * Side effects:
 *      Result is appended to dsPtr.
 *
 *----------------------------------------------------------------------
 */

#if defined(HAVE_GETNAMEINFO)

static bool
GetHost(Ns_DString *dsPtr, const char *addr)
{
    struct sockaddr_in sa;
    char buf[NI_MAXHOST];
    int result;
    bool status = NS_FALSE;
#ifndef HAVE_MTSAFE_DNS
    static Ns_Cs cs;
    Ns_CsEnter(&cs);
#endif

    memset(&sa, 0, sizeof(struct sockaddr_in));

#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
    sa.sin_len = sizeof(struct sockaddr_in);
#endif
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr(addr);

    result = getnameinfo((const struct sockaddr *) &sa,
                         sizeof(struct sockaddr_in), buf, sizeof(buf),
                         NULL, 0u, NI_NAMEREQD);
    if (result == 0) {
        Ns_DStringAppend(dsPtr, buf);
        status = NS_TRUE;
    } else if (result != EAI_NONAME) {
        Ns_Log(Warning, "dns: getnameinfo failed: %s (%s)", gai_strerror(result), addr);
    }
#ifndef HAVE_MTSAFE_DNS
    Ns_CsLeave(&cs);
#endif

    return status;
}

#elif defined(HAVE_GETHOSTBYADDR_R)

static bool
GetHost(Ns_DString *dsPtr, const char *addr)
{
    struct hostent he, *hePtr;
    struct sockaddr_in sa;
    char buf[2048];
    int h_errnop;
    bool status = NS_FALSE;

    sa.sin_addr.s_addr = inet_addr(addr);
    hePtr = gethostbyaddr_r((char *) &sa.sin_addr, sizeof(struct in_addr),
                AF_INET, &he, buf, sizeof(buf), &h_errnop);
    if (hePtr == NULL) {
        LogError("gethostbyaddr_r", h_errnop);
    } else if (he.h_name != NULL) {
        Ns_DStringAppend(dsPtr, he.h_name);
        status = NS_TRUE;
    }
    return status;
}

#else

/*
 * This version is not thread-safe, but we have no thread-safe
 * alternative on this platform.  Use critsec to try and serialize
 * calls, but beware: Tcl core as of 8.4.6 still calls gethostbyaddr()
 * as well, so it's still possible for two threads to call it at
 * the same time.
 */

static bool
GetHost(Ns_DString *dsPtr, const char *addr)
{
    struct sockaddr_in sa;
    static Ns_Cs cs;
    bool status = NS_FALSE;

    sa.sin_addr.s_addr = inet_addr(addr);
    if (sa.sin_addr.s_addr != INADDR_NONE) {
        struct hostent *he;

        Ns_CsEnter(&cs);
        he = gethostbyaddr((char *) &sa.sin_addr,
                           sizeof(struct in_addr), AF_INET);
        if (he == NULL) {
            LogError("gethostbyaddr", h_errno);
        } else if (he->h_name != NULL) {
            Ns_DStringAppend(dsPtr, he->h_name);
            status = NS_TRUE;
        }
        Ns_CsLeave(&cs);
    }
    return status;
}
#endif


#if defined(HAVE_GETADDRINFO)
static bool
GetAddr(Ns_DString *dsPtr, const char *host)
{
    struct addrinfo hints;
    struct addrinfo *res, *ptr;
    int result;
    bool status = NS_FALSE;
#ifndef HAVE_MTSAFE_DNS
    static Ns_Cs cs;
    Ns_CsEnter(&cs);
#endif

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_INET;
    hints.ai_socktype = SOCK_STREAM;

    result = getaddrinfo(host, NULL, &hints, &res);
    if (result == 0) {
        ptr = res;
        while (ptr != NULL) {
            char ipString[NS_IPADDR_SIZE];
            
            ns_inet_ntop(ptr->ai_addr, ipString, sizeof(ipString));
            Tcl_DStringAppendElement(dsPtr, ipString);
            status = NS_TRUE;
            ptr = ptr->ai_next;
        }
        freeaddrinfo(res);
    } else if (result != EAI_NONAME) {
        Ns_Log(Error, "dns: getaddrinfo failed for %s: %s", host,
               gai_strerror(result));
    }
#ifndef HAVE_MTSAFE_DNS
    Ns_CsLeave(&cs);
#endif
    return status;
}

#elif defined(HAVE_GETHOSTBYNAME_R)

static bool
GetAddr(Ns_DString *dsPtr, const char *host)
{
    struct in_addr ia, *ptr;
    char buf[2048];
    int result = 0;
    int h_errnop = 0;
    bool status = NS_FALSE;
#if defined(HAVE_GETHOSTBYNAME_R_6) || defined(HAVE_GETHOSTBYNAME_R_5)
    struct hostent he;
#endif
#ifdef HAVE_GETHOSTBYNAME_R_3
    struct hostent_data data;
#endif


    memset(buf, 0, sizeof(buf));

#if defined(HAVE_GETHOSTBYNAME_R_6)
    result = gethostbyname_r(host, &he, buf, sizeof(buf), &res, &h_errnop);
#elif defined(HAVE_GETHOSTBYNAME_R_5)
    {
      struct hostent *res;

      res = gethostbyname_r(host, &he, buf, sizeof(buf), &h_errnop);
      if (res == NULL) {
        result = -1;
      }
    }
#elif defined(HAVE_GETHOSTBYNAME_R_3)
    result = gethostbyname_r(host, &he, &data);
    h_errnop = h_errno;
#endif

    if (result != 0) {
        LogError("gethostbyname_r", h_errnop);
    } else {
        int i = 0;
        while ((ptr = (struct in_addr *) he.h_addr_list[i++]) != NULL) {
            /*
             * This is legacy code and works probably just under IPv4, IPv6 requires
             * HAVE_GETADDRINFO.
             */
            ia.s_addr = ptr->s_addr;
            Tcl_DStringAppendElement(dsPtr, ns_inet_ntoa((struct sockaddr *)(ptr->ai_addr)));
            status = NS_TRUE;
        }
    }
    return status;
}

#else

/*
 * This version is not thread-safe, but we have no thread-safe
 * alternative on this platform.  Use critsec to try and serialize
 * calls, but beware: Tcl core as of 8.4.6 still calls gethostbyname()
 * as well, so it's still possible for two threads to call it at
 * the same time.
 */

static bool
GetAddr(Ns_DString *dsPtr, const char *host)
{
    struct hostent *he;
    struct in_addr ia, *ptr;
    static Ns_Cs cs;
    bool status = NS_FALSE;

    Ns_CsEnter(&cs);
    he = gethostbyname(host);
    if (he == NULL) {
        LogError("gethostbyname", h_errno);
    } else {
        int i = 0;
        while ((ptr = (struct in_addr *) he->h_addr_list[i++]) != NULL) {
            /*
             * This is legacy code and works probably just under IPv4, IPv6 requires
             * HAVE_GETADDRINFO.
             */
            ia.s_addr = ptr->s_addr;
            Tcl_DStringAppendElement(dsPtr, ns_inet_ntoa((struct sockaddr *)(ptr->ai_addr)));
            status = NS_TRUE;
        }
    }
    Ns_CsLeave(&cs);

    return status;
}

#endif

/* 
 * End no IPv6
 */

#endif /* HAVE_IPV6 */







/*
 *----------------------------------------------------------------------
 * LogError -
 *
 *      Log errors which may indicate a failure in the underlying
 *      resolver.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETNAMEINFO)

static void
LogError(char *func, int h_errnop)
{
    char        buf[100];
    const char *h, *e;

    e = NULL;
    switch (h_errnop) {
    case HOST_NOT_FOUND:
        /* Log nothing. */
        return;
        break;

    case TRY_AGAIN:
        h = "temporary error - try again";
        break;

    case NO_RECOVERY:
        h = "unexpected server failure";
        break;

    case NO_DATA:
        h = "no valid IP address";
        break;

#ifdef NETDB_INTERNAL
    case NETDB_INTERNAL:
        h = "netdb internal error: ";
        e = strerror(errno);
        break;
#endif

    default:
        snprintf(buf, sizeof(buf), "unknown error #%d", h_errnop);
        h = buf;
    }

    Ns_Log(Error, "dns: %s failed: %s%s", func, h, (e != 0) ? e : "");
}

#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
