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
 * dns.c --
 *
 *      DNS lookup routines.
 */

#include "nsd.h"

/*
 * W2000 has no getaddrinfo, requires special headers for inline functions
 */

#if defined _MSC_VER || defined __MINGW__
#  include <ws2tcpip.h>
/*#  include <Wspiapi.h>*/
#endif

#ifndef NETDB_INTERNAL
#  ifdef h_NETDB_INTERNAL
#  define NETDB_INTERNAL h_NETDB_INTERNAL
#  endif
#endif

#ifdef NEED_HERRNO
extern int h_errno;
#endif


typedef bool (GetProc)(Tcl_DString *dsPtr, const char *key);

#if defined(HAVE_IPV6) && defined(HAVE_GETADDRINFO)
typedef struct DnsJob DnsJob;

struct Ns_DnsRequest {
    DnsJob               *jobPtr;
    struct Ns_DnsRequest *nextPtr;
    NS_SOCKET             pipe[2];
    bool                  seen[2];
};

struct DnsJob {
    DnsJob       *nextPtr;
    char         *host;
    Tcl_DString   addresses[2];
    unsigned int  state[3];       /* 0 queued, 1 running, 2 finished. */
    unsigned int  remaining;
    Ns_DnsRequest *requests;
};

static Ns_Mutex dnsLock;
static Ns_Cond  dnsCond;
static DnsJob *firstJob;
static unsigned int jobCount, workerCount;
static bool dnsStarted, dnsStopping;
static int workerKinds[] = {0, 0, 1, 1, 2};

static Ns_ThreadProc DnsWorker;
static Ns_ShutdownProc DnsStop;
static void DnsFreeJob(DnsJob *jobPtr) NS_GNUC_NONNULL(1);
#endif /* HAVE_IPV6 && HAVE_GETADDRINFO */


/*
 * Static functions defined in this file
 */

static GetProc GetAddr;
static GetProc GetHost;
#ifdef HAVE_IPV6
static bool GetAddrFamily(Tcl_DString *dsPtr, const char *host, int family)
    NS_GNUC_NONNULL(1,2);
#endif
static void DnsInitAsync(void);
static bool DnsGet(GetProc *getProc, Tcl_DString *dsPtr,
                   Ns_Cache *cache, const char *key, bool all)
    NS_GNUC_NONNULL(1,2,4);


#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETNAMEINFO)
static void LogError(char *func, int h_errnop);
#endif


/*
 * Static variables defined in this file
 */

static Ns_Cache *hostCache;
static Ns_Cache *addrCache;
static Ns_Time   ttl;       /* Time in seconds each entry can live in the cache. */
static Ns_Time   timeout = {5, 0};   /* Time in seconds to wait for concurrent update.  */
static Ns_Cs     getDNScs = NULL;



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
 *      Further DNS lookups will be cached using given ttl.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigDNS(void)
{
    const char *section = NS_GLOBAL_CONFIG_PARAMETERS;

    Ns_ConfigTimeUnitRange(section, "dnswaittimeout", "5s", 0, 0, INT_MAX, 0,
                           &timeout);
    if (Ns_ConfigBool(section, "dnscache", NS_TRUE) == NS_TRUE) {
        size_t maxSize = (size_t)Ns_ConfigMemUnitRange(section, "dnscachemaxsize", "500KB", (Tcl_WideInt)1024 * 500,
                                                       0, INT_MAX);
        if (maxSize > 0u) {
            Ns_ConfigTimeUnitRange(section, "dnscachetimeout",
                                   "60m", 0, 0, INT_MAX, 0,
                                   &ttl);

            hostCache = Ns_CacheCreateSz("ns:dnshost", TCL_STRING_KEYS,
                                         maxSize, ns_free);
            addrCache = Ns_CacheCreateSz("ns:dnsaddr", TCL_STRING_KEYS,
                                         maxSize, ns_free);
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
 *      address not found. An error message may be logged if not
 *      found.
 *
 * Side effects:
 *      Result may be cached.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_GetHostByAddr(Tcl_DString *dsPtr, const char *addr)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(addr != NULL);

    return DnsGet(GetHost, dsPtr, hostCache, addr, NS_FALSE);
}

bool
Ns_GetAddrByHost(Tcl_DString *dsPtr, const char *host)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(host != NULL);

    return DnsGet(GetAddr, dsPtr, addrCache, host, NS_FALSE);
}


bool
Ns_GetAllAddrByHost(Tcl_DString *dsPtr, const char *host)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(host != NULL);

    return DnsGet(GetAddr, dsPtr, addrCache, host, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * NsDnsGetCached --
 *
 *      Read a complete existing address-cache entry without waiting for a
 *      refresh. Preserve the combined cache's value format and expiry.
 *
 * Results:
 *      NS_TRUE for a cache hit, NS_FALSE otherwise.
 *
 * Side effects:
 *      Copies cached addresses, the resolver wait budget, and cache state.
 *
 *----------------------------------------------------------------------
 */
bool
NsDnsGetCached(const char *host, Tcl_DString *dsPtr, Ns_Time *waitPtr,
                bool *cacheEnabledPtr)
{
    bool found = NS_FALSE;

    *waitPtr = timeout;
    *cacheEnabledPtr = addrCache != NULL;
    if (addrCache != NULL) {
        Ns_Entry *entry;

        Ns_CacheLock(addrCache);
        entry = Ns_CacheFindEntry(addrCache, host);
        if (entry != NULL) {
            Tcl_DStringAppend(dsPtr, Ns_CacheGetValue(entry),
                              (TCL_SIZE_T)Ns_CacheGetSize(entry));
            found = NS_TRUE;
        }
        Ns_CacheUnlock(addrCache);
    }
    return found;
}


#if defined(HAVE_IPV6) && defined(HAVE_GETADDRINFO)
/*
 *----------------------------------------------------------------------
 *
 * DnsInitAsync --
 *
 *      Initialize the private resolver pool's synchronization once, before
 *      any caller can subscribe. Worker threads are started lazily.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes the mutex and condition variable.
 *
 *----------------------------------------------------------------------
 */
static void
DnsInitAsync(void)
{
    Ns_MutexInit(&dnsLock);
    Ns_MutexSetName2(&dnsLock, "ns:dns", "incremental");
    Ns_CondInit(&dnsCond);
}



/*
 *----------------------------------------------------------------------
 *
 * DnsFreeJob --
 *
 *      Release a completed job after its last subscriber has detached.
 *      The caller holds dnsLock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees the job and its address storage.
 *
 *----------------------------------------------------------------------
 */
static void
DnsFreeJob(DnsJob *jobPtr)
{
    Tcl_DStringFree(&jobPtr->addresses[0]);
    Tcl_DStringFree(&jobPtr->addresses[1]);
    ns_free(jobPtr->host);
    ns_free(jobPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * DnsStop --
 *
 *      Stop accepting work during shutdown. Detached workers drain their
 *      bounded queues. The wait phase obeys the shared shutdown deadline;
 *      a blocked system lookup cannot extend it. Synchronization and cache
 *      objects remain valid until process exit, including after a timeout.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Wakes idle workers.
 *
 *----------------------------------------------------------------------
 */
static void
DnsStop(const Ns_Time *timeoutPtr, void *arg)
{
    (void)arg;
    Ns_MutexLock(&dnsLock);
    dnsStopping = NS_TRUE;
    Ns_CondBroadcast(&dnsCond);
    if (timeoutPtr != NULL) {
        while (workerCount > 0u) {
            if (Ns_CondTimedWait(&dnsCond, &dnsLock, timeoutPtr) != NS_OK) {
                break;
            }
        }
    }
    Ns_MutexUnlock(&dnsLock);
}

/*
 *----------------------------------------------------------------------
 *
 * DnsWorker --
 *
 *      Resolve one address family independently using the system resolver.
 *      Dedicated family workers prevent a slow AAAA lookup from serializing
 *      the A lookup. A separate worker fills the existing combined cache
 *      through its original AF_UNSPEC path, retaining system address order.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Publishes results and wakes subscribers. Never touches TCP sockets.
 *
 *----------------------------------------------------------------------
 */
static void
DnsWorker(void *arg)
{
    int kind = *(const int *)arg;

    Ns_ThreadSetName("-dns:%d-", kind);
    Ns_MutexLock(&dnsLock);
    for (;;) {
        DnsJob *jobPtr;

        for (jobPtr = firstJob; jobPtr != NULL; jobPtr = jobPtr->nextPtr) {
            if (jobPtr->state[kind] == 0u
                && (kind != 2 || (jobPtr->state[0] == 2u && jobPtr->state[1] == 2u))) {
                break;
            }
        }
        if (jobPtr == NULL) {
            if (dnsStopping && firstJob == NULL) {
                break;
            }
            Ns_CondWait(&dnsCond, &dnsLock);
            continue;
        }
        jobPtr->state[kind] = 1u;
        Ns_MutexUnlock(&dnsLock);
        if (kind == 2) {
            Tcl_DString ds;

            Tcl_DStringInit(&ds);
            (void)Ns_GetAllAddrByHost(&ds, jobPtr->host);
            Tcl_DStringFree(&ds);
        } else {
            Tcl_DString ds;

            Tcl_DStringInit(&ds);
            (void)GetAddrFamily(&ds, jobPtr->host, kind == 0 ? AF_INET6 : AF_INET);
            Ns_MutexLock(&dnsLock);
            Tcl_DStringAppend(&jobPtr->addresses[kind], ds.string, ds.length);
            Ns_MutexUnlock(&dnsLock);
            Tcl_DStringFree(&ds);
        }
        Ns_MutexLock(&dnsLock);
        jobPtr->state[kind] = 2u;
        jobPtr->remaining--;
        if (jobPtr->state[0] == 2u && jobPtr->state[1] == 2u
            && jobPtr->state[2] == 0u
            && jobPtr->addresses[0].length == 0 && jobPtr->addresses[1].length == 0) {
            /* There is no positive combined answer to cache. */
            jobPtr->state[2] = 2u;
            jobPtr->remaining--;
        }
        if (kind != 2) {
            Ns_DnsRequest *requestPtr;

            for (requestPtr = jobPtr->requests; requestPtr != NULL;
                 requestPtr = requestPtr->nextPtr) {
                /* Nonblocking, coalesced wakeups; the job is authoritative. */
                int written;

                do {
                    written = (int)send(requestPtr->pipe[1], "d", 1, 0);
                } while (written < 0 && ns_sockerrno == NS_EINTR);
            }
        }
        if (jobPtr->remaining == 0u) {
            DnsJob **linkPtr = &firstJob;

            while (*linkPtr != jobPtr) {
                linkPtr = &(*linkPtr)->nextPtr;
            }
            *linkPtr = jobPtr->nextPtr;
            jobCount--;
            if (jobPtr->requests == NULL) {
                DnsFreeJob(jobPtr);
            }
        }
        Ns_CondBroadcast(&dnsCond);
    }
    workerCount--;
    Ns_CondBroadcast(&dnsCond);
    Ns_MutexUnlock(&dnsLock);
}

/*
 *----------------------------------------------------------------------
 *
 * NsDnsStart --
 *
 *      Return a numeric literal or a complete cached answer immediately.
 *      Otherwise subscribe to independent family lookups, coalescing work
 *      for the same host. Bound running and queued jobs to 64 hostnames.
 *
 * Results:
 *      NS_TRUE on success. A non-NULL request requires incremental polling;
 *      otherwise dsPtr contains the complete answer. NS_FALSE on failure.
 *
 * Side effects:
 *      May start detached resolver workers and allocate a notification pair.
 *      Returns a separate resolver wait budget, never a TCP deadline.
 *
 *----------------------------------------------------------------------
 */
bool
NsDnsStart(const char *host, Tcl_DString *dsPtr, Ns_DnsRequest **requestPtr,
           Ns_Time *waitPtr)
{
    struct NS_SOCKADDR_STORAGE sa;
    Ns_DnsRequest *reqPtr;
    DnsJob        *jobPtr;
    bool           cacheEnabled;

    *requestPtr = NULL;
    if (NsDnsGetCached(host, dsPtr, waitPtr, &cacheEnabled)) {
        return NS_TRUE;
    }
    if (ns_inet_pton((struct sockaddr *)&sa, host) > 0) {
        Tcl_DStringAppend(dsPtr, host, TCL_INDEX_NONE);
        return NS_TRUE;
    }
    reqPtr = ns_calloc(1u, sizeof(Ns_DnsRequest));
    if (ns_sockpair(reqPtr->pipe) != 0) {
        ns_free(reqPtr);
        return NS_FALSE;
    }
    if (Ns_SockSetNonBlocking(reqPtr->pipe[0]) != NS_OK
        || Ns_SockSetNonBlocking(reqPtr->pipe[1]) != NS_OK) {
        ns_sockerrno_t error = ns_sockerrno;

        ns_sockclose(reqPtr->pipe[0]);
        ns_sockclose(reqPtr->pipe[1]);
        ns_free(reqPtr);
        Ns_SetSockErrno(error);
        return NS_FALSE;
    }
    Ns_MutexLock(&dnsLock);
    for (jobPtr = firstJob; jobPtr != NULL; jobPtr = jobPtr->nextPtr) {
        if (strcmp(host, jobPtr->host) == 0) {
            break;
        }
    }
    if (jobPtr == NULL && jobCount >= 64u) {
        DnsJob **linkPtr = &firstJob;

        /* Cache warming must not starve family lookups if its worker stalls. */
        while (*linkPtr != NULL) {
            DnsJob *oldPtr = *linkPtr;

            if (oldPtr->remaining == 1u && oldPtr->state[2] == 0u
                && oldPtr->requests == NULL) {
                *linkPtr = oldPtr->nextPtr;
                jobCount--;
                DnsFreeJob(oldPtr);
                break;
            }
            linkPtr = &oldPtr->nextPtr;
        }
    }
    if (dnsStopping || (jobPtr == NULL && jobCount >= 64u)) {
        Ns_MutexUnlock(&dnsLock);
        ns_sockclose(reqPtr->pipe[0]);
        ns_sockclose(reqPtr->pipe[1]);
        ns_free(reqPtr);
        Ns_SetSockErrno(NS_EAGAIN);
        return NS_FALSE;
    }
    if (jobPtr == NULL) {
        DnsJob **linkPtr = &firstJob;

        jobPtr = ns_calloc(1u, sizeof(DnsJob));
        jobPtr->host = ns_strdup(host);
        Tcl_DStringInit(&jobPtr->addresses[0]);
        Tcl_DStringInit(&jobPtr->addresses[1]);
        jobPtr->state[2] = cacheEnabled ? 0u : 2u;
        jobPtr->remaining = cacheEnabled ? 3u : 2u;
        while (*linkPtr != NULL) {
            linkPtr = &(*linkPtr)->nextPtr;
        }
        *linkPtr = jobPtr;
        jobCount++;
    }
    reqPtr->jobPtr = jobPtr;
    reqPtr->nextPtr = jobPtr->requests;
    jobPtr->requests = reqPtr;
    *requestPtr = reqPtr;
    if (!dnsStarted) {
        dnsStarted = NS_TRUE;
        workerCount = (unsigned int)Ns_NrElements(workerKinds);
        (void)Ns_RegisterAtShutdown(DnsStop, NULL);
        for (size_t i = 0u; i < Ns_NrElements(workerKinds); i++) {
            Ns_ThreadCreate(DnsWorker, &workerKinds[i], 0, NULL);
        }
    }
    Ns_CondBroadcast(&dnsCond);
    Ns_MutexUnlock(&dnsLock);
    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * NsDnsResults --
 *
 *      Collect each completed family's results once for this subscriber.
 *      Completion without addresses is distinct from a pending lookup.
 *
 * Results:
 *      The notification socket for use in the caller's poll loop.
 *
 * Side effects:
 *      Drains wakeups, appends new results, and updates completion flags.
 *
 *----------------------------------------------------------------------
 */
NS_SOCKET
NsDnsResults(Ns_DnsRequest *requestPtr, Tcl_DString *v6Ptr, Tcl_DString *v4Ptr,
             bool *done6Ptr, bool *done4Ptr)
{
    char         buffer[32];
    Tcl_DString *results[] = {v6Ptr, v4Ptr};

    while (recv(requestPtr->pipe[0], buffer, sizeof(buffer), 0) > 0) {
        /* Drain coalesced notifications. */
    }
    Ns_MutexLock(&dnsLock);
    for (int i = 0; i < 2; i++) {
        if (!requestPtr->seen[i] && requestPtr->jobPtr->state[i] == 2u) {
            const Tcl_DString *dsPtr = &requestPtr->jobPtr->addresses[i];

            Tcl_DStringAppend(results[i], dsPtr->string, dsPtr->length);
            requestPtr->seen[i] = NS_TRUE;
        }
    }
    *done6Ptr = requestPtr->seen[0];
    *done4Ptr = requestPtr->seen[1];
    Ns_MutexUnlock(&dnsLock);
    return requestPtr->pipe[0];
}


/*
 *----------------------------------------------------------------------
 *
 * NsDnsCancel --
 *
 *      Detach a subscriber without waiting for system resolver calls.
 *      Notifications and socket closure share dnsLock, preventing writes
 *      after close, descriptor reuse races, and SIGPIPE on the wakeup pair.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Closes notification sockets and releases completed unreferenced jobs.
 *
 *----------------------------------------------------------------------
 */
void
NsDnsCancel(Ns_DnsRequest *requestPtr)
{
    DnsJob *jobPtr = requestPtr->jobPtr;
    Ns_DnsRequest **linkPtr;

    Ns_MutexLock(&dnsLock);
    linkPtr = &jobPtr->requests;
    while (*linkPtr != requestPtr) {
        linkPtr = &(*linkPtr)->nextPtr;
    }
    *linkPtr = requestPtr->nextPtr;
    ns_sockclose(requestPtr->pipe[0]);
    ns_sockclose(requestPtr->pipe[1]);
    if (jobPtr->remaining == 0u && jobPtr->requests == NULL) {
        DnsFreeJob(jobPtr);
    }
    Ns_MutexUnlock(&dnsLock);
    ns_free(requestPtr);
}

#else

/*
 * Legacy resolver builds retain synchronous lookup through the existing API.
 * No workers or notification sockets are allocated in these builds.
 */
static void
DnsInitAsync(void)
{
}

bool
NsDnsStart(const char *host, Tcl_DString *dsPtr, Ns_DnsRequest **requestPtr,
           Ns_Time *waitPtr)
{
    *requestPtr = NULL;
    *waitPtr = timeout;
    return Ns_GetAllAddrByHost(dsPtr, host);
}

NS_SOCKET
NsDnsResults(Ns_DnsRequest *requestPtr, Tcl_DString *v6Ptr, Tcl_DString *v4Ptr,
             bool *done6Ptr, bool *done4Ptr)
{
    (void)requestPtr;
    (void)v6Ptr;
    (void)v4Ptr;
    *done6Ptr = NS_TRUE;
    *done4Ptr = NS_TRUE;
    return NS_INVALID_SOCKET;
}

void
NsDnsCancel(Ns_DnsRequest *requestPtr)
{
    (void)requestPtr;
}
#endif /* HAVE_IPV6 && HAVE_GETADDRINFO */


static bool
DnsGet(GetProc *getProc, Tcl_DString *dsPtr, Ns_Cache *cache, const char *key, bool all)
{
    Tcl_DString ds;
    Ns_Time     t;
    int         isNew;
    bool        success;

    NS_NONNULL_ASSERT(getProc != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    /*
     * Call getProc directly or through cache.
     */

    Tcl_DStringInit(&ds);
    if (cache == NULL) {
        success = (*getProc)(&ds, key);
    } else {
        Ns_Entry   *entry;

        Ns_GetTime(&t);
        Ns_IncrTime(&t, timeout.sec, timeout.usec);

        Ns_CacheLock(cache);
        entry = Ns_CacheWaitCreateEntry(cache, key, &isNew, &t);
        if (entry == NULL) {
            Ns_CacheUnlock(cache);
            Ns_Log(Notice, "dns: timeout waiting for concurrent update");
            success = NS_FALSE;

        } else {
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
                    Ns_IncrTime(&endTime, ttl.sec, ttl.usec);
                    Ns_CacheSetValueExpires(entry, ns_strdup(ds.string),
                                            (size_t)ds.length, &endTime,
                                            (int)(diffTime.sec * 1000000 + diffTime.usec),
                                            0u, 0u);
                }
                Ns_CacheBroadcast(cache);
            } else {
                Tcl_DStringAppend(&ds, Ns_CacheGetValue(entry),
                                  (TCL_SIZE_T)Ns_CacheGetSize(entry));
                success = NS_TRUE;
            }
            Ns_CacheUnlock(cache);
        }

    }

    if (success) {
        if (getProc == GetAddr && !all) {
            /*
             * When "all" is not specified for a GetAddr() call,
             * return just the first address.
             */
            const char *p = ds.string;

            while (*p != '\0' && CHARTYPE(space, *p) == 0) {
                ++p;
            }
            Tcl_DStringSetLength(&ds, (TCL_SIZE_T)(p - ds.string));
        }
        Tcl_DStringAppend(dsPtr, ds.string, ds.length);
    }
    Tcl_DStringFree(&ds);

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
 *      If a name can be found, the function returns NS_TRUE;
 *      otherwise, it returns NS_FALSE.
 *
 * Side effects:
 *      Result is appended to dsPtr.
 *
 *----------------------------------------------------------------------
 */

static bool
GetHost(Tcl_DString *dsPtr, const char *addr)
{
    int    r;
    struct sockaddr_storage sa;
    struct sockaddr        *saPtr = (struct sockaddr *)&sa;
    bool   success = NS_FALSE;

    sa.ss_family = AF_UNSPEC;
    r = ns_inet_pton(saPtr, addr);
    if (r > 0) {
        char buf[NI_MAXHOST];
        int  err;

        err = getnameinfo(saPtr,
                          ((sa.ss_family == AF_INET6)
                           ? (socklen_t)sizeof(struct sockaddr_in6)
                           : (socklen_t)sizeof(struct sockaddr_in)),
                          buf, sizeof(buf),
                          NULL, 0, NI_NAMEREQD);
        if (err != 0) {
            switch (err) {
#if defined(EAI_SYSTEM)
            case EAI_SYSTEM:
                Ns_Log(Warning, "dns: getnameinfo failed for addr <%s>: %s", addr,
                       strerror(errno));
                break;
#endif
            case EAI_NONAME:
                    /*
                     * EAI_NONAME: The name does not resolve for the
                     * supplied arguments. No need to report this as
                     * an error.
                     */
                break;
            default:
                Ns_Log(Warning, "dns: getnameinfo failed for addr <%s>: %s", addr,
                       gai_strerror(err));
            }
        } else {
            Tcl_DStringAppend(dsPtr, buf, TCL_INDEX_NONE);
            success = NS_TRUE;
        }
    }

    return success;
}

static bool
GetAddr(Tcl_DString *dsPtr, const char *host)
{
    return GetAddrFamily(dsPtr, host, AF_UNSPEC);
}


/*
 *----------------------------------------------------------------------
 *
 * GetAddrFamily --
 *
 *      Resolve through the system name service, retaining its order and
 *      AI_ADDRCONFIG policy. AF_UNSPEC is the existing combined-cache path;
 *      AF_INET6 and AF_INET allow independent completion in private workers.
 *      A missing family is expected during racing and is logged at Debug.
 *
 * Results:
 *      NS_TRUE when addresses were found, NS_FALSE otherwise.
 *
 * Side effects:
 *      Appends numeric addresses to dsPtr.
 *
 *----------------------------------------------------------------------
 */
static bool
GetAddrFamily(Tcl_DString *dsPtr, const char *host, int family)
{
    struct       addrinfo  hints;
    const struct addrinfo *ptr;
    struct       addrinfo *res;
    int          result;
    bool         success = NS_FALSE;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_STREAM;

    result = getaddrinfo(host, NULL, &hints, &res);
    switch (result) {
    case 0:
        ptr = res;
        while (ptr != NULL) {

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

                success = NS_FALSE;
                break;
            } else {
                char ipString[NS_IPADDR_SIZE];

                Tcl_DStringAppendElement(dsPtr,
                                         ns_inet_ntop(ptr->ai_addr, ipString, sizeof(ipString)));

                success = NS_TRUE;
                ptr = ptr->ai_next;
            }
        }
        freeaddrinfo(res);
        break;

#if defined(EAI_SYSTEM)
        case EAI_SYSTEM:
            Ns_Log(family == AF_UNSPEC ? Warning : Debug, "dns: getaddrinfo failed for %s: %s", host,
                   strerror(errno));
            break;
#endif
    case EAI_NONAME:
        /*
         * EAI_NONAME: The name does not resolve for the supplied arguments
         */
        break;

    default:
        Ns_Log(family == AF_UNSPEC ? Warning : Debug, "dns: getaddrinfo failed for %s: %s", host,
               gai_strerror(result));
        break;
    }

    return success;
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
 *      NOTE: A critical section is used instead of a mutex to ensure
 *      waiting on a condition and not mutex spin waiting.
 *
 * Results:
 *      If a name can be found, the function returns NS_TRUE;
 *      otherwise, it returns NS_FALSE.
 *
 * Side effects:
 *      Result is appended to dsPtr.
 *
 *----------------------------------------------------------------------
 */

#if defined(HAVE_GETNAMEINFO)

static bool
GetHost(Tcl_DString *dsPtr, const char *addr)
{
    struct sockaddr_in sa;
    char buf[NI_MAXHOST];
    int result;
    bool status = NS_FALSE;
#ifndef HAVE_MTSAFE_DNS
    Ns_CsEnter(&getDNScs);
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
    } else if (result == EAI_SYSTEM) {
        Ns_Log(Warning, "dns: getnameinfo failed: %s (%s)",
               strerror(errno), addr);
    } else if (result != EAI_NONAME) {
        Ns_Log(Warning, "dns: getnameinfo failed: %s (%s)",
               gai_strerror(result), addr);
    } else {
        /*
         * EAI_NONAME: The name does not resolve for the supplied arguments
         */
    }
#ifndef HAVE_MTSAFE_DNS
    Ns_CsLeave(&getDNScs);
#endif

    return status;
}

#elif defined(HAVE_GETHOSTBYADDR_R)

static bool
GetHost(Tcl_DString *dsPtr, const char *addr)
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
 * as well, so it is still possible for two threads to call it at
 * the same time.
 */

#ifndef INADDR_NONE
#define INADDR_NONE (-1)
#endif

static bool
GetHost(Tcl_DString *dsPtr, const char *addr)
{
    struct sockaddr_in sa;
    bool status = NS_FALSE;

    sa.sin_addr.s_addr = inet_addr(addr);
    if (sa.sin_addr.s_addr != INADDR_NONE) {
        struct hostent *he;

        Ns_CsEnter(&getDNScs);
        he = gethostbyaddr((char *) &sa.sin_addr,
                           sizeof(struct in_addr), AF_INET);
        if (he == NULL) {
            LogError("gethostbyaddr", h_errno);
        } else if (he->h_name != NULL) {
            Ns_DStringAppend(dsPtr, he->h_name);
            status = NS_TRUE;
        }
        Ns_CsLeave(&getDNScs);
    }
    return status;
}
#endif


#if defined(HAVE_GETADDRINFO)
static bool
GetAddr(Tcl_DString *dsPtr, const char *host)
{
    struct addrinfo  hints;
    struct addrinfo *res, *ptr;
    int              result;
    bool             status = NS_FALSE;
#ifndef HAVE_MTSAFE_DNS

    Ns_CsEnter(&getDNScs);
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
    } else if (result == EAI_SYSTEM) {
        Ns_Log(Warning, "dns: getaddrinfo failed for %s: %s", host,
               strerror(errno));
    } else if (result != EAI_NONAME) {
        Ns_Log(Warning, "dns: getaddrinfo failed for %s: %s", host,
               gai_strerror(result));
    } else {
        /*
         * EAI_NONAME: The name does not resolve for the supplied arguments
         */
    }
#ifndef HAVE_MTSAFE_DNS
    Ns_CsLeave(&getDNScs);
#endif
    return status;
}

#elif defined(HAVE_GETHOSTBYNAME_R)

static bool
GetAddr(Tcl_DString *dsPtr, const char *host)
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
 * as well, so it is still possible for two threads to call it at
 * the same time.
 */

static bool
GetAddr(Tcl_DString *dsPtr, const char *host)
{
    struct hostent *he;
    struct in_addr ia, *ptr;
    bool status = NS_FALSE;

    Ns_CsEnter(&getDNScs);
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
    Ns_CsLeave(&getDNScs);

    return status;
}

#endif

/*
 * End no IPv6
 */

#endif /* HAVE_IPV6 */


/*
 *----------------------------------------------------------------------
 *
 * NsHostnameIsNumericIP --
 *
 *      Check, of the specified hostname is a numeric IP address
 *      (IPv4 or IPv6).
 *
 * Results:
 *      Boolean.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
NsHostnameIsNumericIP(const char *hostname)
{
    bool        result = NS_TRUE;

    NS_NONNULL_ASSERT(hostname != NULL);

    /*
     * When the hostname contains a colon, it must be an IPv6
     * address. It will be regarded as numeric.
     */
    if (strchr(hostname, INTCHAR(':')) == NULL) {
        /*
         * Otherwise, make sure, the name contains only digits or
         * dots.
         */
        const char *p = hostname;

        while (*p != 0) {
            char c = *p;

            if ((c >= '0' && c <= '9') || c == '.') {
                p++;
                continue;
            }
            result = NS_FALSE;
            break;
        }
    }

    return result;
}



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
    const char *h, *e = NULL;

    switch (h_errnop) {
    case HOST_NOT_FOUND:
        /* Log nothing. */
        h = NULL;
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
        errorStr = strerror(errno);
        break;
#endif

    default:
        snprintf(buf, sizeof(buf), "unknown error #%d", h_errnop);
        h = buf;
    }

    if (h != NULL) {
        Ns_Log(Error, "dns: %s failed: %s%s", func, h,
               (errorStr != NULL) ? errorStr : NS_EMPTY_STRING);
    }
}

#endif

/*
 *----------------------------------------------------------------------
 *
 * NsInitDNS --
 *
 *      Initialize once the critical section.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      One-time initialization.
 *
 *----------------------------------------------------------------------
 */
void NsInitDNS(void) {
    //fprintf(stderr, "==== NsInitDNS =====================================\n");
    Ns_CsInit(&getDNScs);
    DnsInitAsync();
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
