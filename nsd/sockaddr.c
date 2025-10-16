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

#include "nsd.h"

/*
 * sockaddr.c --
 *
 *      Generic Interface for IPv4 and IPv6
 */

static const char *nonPublicCIDR[] = {
    /*
     * Private network addresses
     */
    "10.0.0.0/8",
    "172.16.0.0/12",
    "192.168.0.0/16",
    "fd00::/8",

    /*
     * Private loopback addresses
     */
    "127.0.0.0/8",
    "::1/128",

    /*
     * Link-local addresses
     */
    "69.254.0.0/16",
    "fe80::/10",

    /*
     * Current network
     */
    "0.0.0.0/8",
    "::/128",
    NULL
};

typedef struct MaskedEntry {
    const char *cdirString;
    struct NS_SOCKADDR_STORAGE mask;
    struct NS_SOCKADDR_STORAGE masked;
} MaskedEntry;

static MaskedEntry *trustedServersEntries = NULL;
static MaskedEntry *nonPublicEntries = NULL;


static void SockkAddrInitMaskedEntry(const char *cdirString, MaskedEntry *entryPtr, const char *errorString)
    NS_GNUC_NONNULL(1)  NS_GNUC_NONNULL(2)  NS_GNUC_NONNULL(3);

static bool SockAddrInit(void);


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrMask --
 *
 *      Compute from  "addr" and "mask" a "maskedAddr" in a generic way
 *      (for IPv4 and IPv6 addresses).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The last argument (maskedAddr) is updated.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SockaddrMask(const struct sockaddr *addr, const struct sockaddr *mask, struct sockaddr *maskedAddr)
{
    bool success = NS_TRUE;

    NS_NONNULL_ASSERT(addr != NULL);
    NS_NONNULL_ASSERT(mask != NULL);
    NS_NONNULL_ASSERT(maskedAddr != NULL);

    /*
     * Copy the full content to maskedAddr in case it is not identical.
     */
    if (addr != maskedAddr) {
        memcpy(maskedAddr, addr, sizeof(struct NS_SOCKADDR_STORAGE));
    }

    if (addr->sa_family == AF_INET6 && mask->sa_family == AF_INET6) {
        const size_t   off = offsetof(struct sockaddr_in6, sin6_addr);
        uint8_t       *dst = (uint8_t *)maskedAddr + off;
        const uint8_t *src = (const uint8_t *)addr + off;
        const uint8_t *msk = (const uint8_t *)mask + off;

        /*
         * Perform bitwise masking over the full array. Maybe we need
         * something special for IN6_IS_ADDR_V4MAPPED.
         */
        {
            uint64_t a0, a1, m0, m1;

            memcpy(&a0, src,      8);
            memcpy(&a1, src +  8, 8);
            memcpy(&m0, msk,      8);
            memcpy(&m1, msk +  8, 8);
            a0 &= m0;
            a1 &= m1;
            memcpy(dst,      &a0, 8);
            memcpy(dst +  8, &a1, 8);
        }
        /*
          fprintf(stderr, "#### addr   %s\n", ns_inet_ntoa(addr));
          fprintf(stderr, "#### mask   %s\n", ns_inet_ntoa(mask));
          fprintf(stderr, "#### masked %s\n", ns_inet_ntoa(maskedAddr));
        */
    } else if (addr->sa_family == AF_INET && mask->sa_family == AF_INET) {
        const size_t off4 = offsetof(struct sockaddr_in, sin_addr);
        uint32_t a, m, o;
        memcpy(&a, (const uint8_t *)addr + off4, 4);
        memcpy(&m, (const uint8_t *)mask + off4, 4);
        o = a & m;
        memcpy((uint8_t *)maskedAddr + off4, &o, 4);

    } else if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
        Ns_Log(Debug, "SockaddrMask: invalid address family %d detected (Ns_SockaddrMask addr)", addr->sa_family);
        success = NS_FALSE;
    } else if (mask->sa_family != AF_INET && mask->sa_family != AF_INET6) {
        Ns_Log(Debug, "SockaddrMask: invalid address family %d detected (Ns_SockaddrMask mask)", mask->sa_family);
        success = NS_FALSE;
    }
    return success;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrSameIP --
 *
 *      Check, if to sockaddrs refer to the same IP address
 *      (for IPv4 and IPv6 addresses).
 *
 * Results:
 *      Boolean expressing success.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrSameIP(const struct sockaddr *addr1, const struct sockaddr *addr2)
{
    NS_NONNULL_ASSERT(addr1 != NULL);
    NS_NONNULL_ASSERT(addr2 != NULL);

    if (addr1 == addr2) {
        return NS_TRUE;

    } else if (addr1->sa_family == AF_INET6 && addr2->sa_family == AF_INET6) {
        const size_t off6 = offsetof(struct sockaddr_in6, sin6_addr);

        return memcmp((const char *)addr1 + off6,
                      (const char *)addr2 + off6,
                      sizeof(struct in6_addr)) == 0 ? NS_TRUE : NS_FALSE;

    } else if (addr1->sa_family == AF_INET && addr2->sa_family == AF_INET) {
        const size_t off4 = offsetof(struct sockaddr_in, sin_addr);

        return memcmp((const char *)addr1 + off4,
                      (const char *)addr2 + off4,
                      sizeof(struct in_addr)) == 0 ? NS_TRUE : NS_FALSE;

#if defined(IN6_IS_ADDR_V4MAPPED)
    } else if (addr1->sa_family == AF_INET && addr2->sa_family == AF_INET6) {
        /* treat IPv4 and v4-mapped IPv6 as equal */
        const size_t off4 = offsetof(struct sockaddr_in,  sin_addr);
        const size_t off6 = offsetof(struct sockaddr_in6, sin6_addr);
        const struct in6_addr *a6 = (const void *)((const char *)addr2 + off6);

        if (IN6_IS_ADDR_V4MAPPED(a6)) {
            return memcmp((const char *)addr1 + off4,
                          ((const char *)a6) + 12, /* last 4 bytes */
                          4) == 0 ? NS_TRUE : NS_FALSE;
        }
        return NS_FALSE;
    }
    if (addr1->sa_family == AF_INET6 && addr2->sa_family == AF_INET) {
        const size_t off4 = offsetof(struct sockaddr_in,  sin_addr);
        const size_t off6 = offsetof(struct sockaddr_in6, sin6_addr);
        const struct in6_addr *a6 = (const void *)((const char *)addr1 + off6);
        if (IN6_IS_ADDR_V4MAPPED(a6)) {
            return memcmp(((const char *)a6) + 12,
                          (const char *)addr2 + off4,
                          4) == 0 ? NS_TRUE : NS_FALSE;
        }
        return NS_FALSE;
    }
#endif
    /*
     * Family mismatch.
     */
    return NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrMaskedMatch --
 *
 *      Check, the provided IPv4 or IPv6 address matches the provided mask and
 *      masked address.
 *
 * Results:
 *      Boolean expressing success.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrMaskedMatch(const struct sockaddr *addr, const struct sockaddr *mask,
                       const struct sockaddr *masked)
{
    NS_NONNULL_ASSERT(addr != NULL);
    NS_NONNULL_ASSERT(mask != NULL);
    NS_NONNULL_ASSERT(masked != NULL);

    //fprintf(stderr, "addr family %d mask family %d\n", addr->sa_family, mask->sa_family);

    if (addr == mask) {
        return NS_TRUE;
    }

    if (addr->sa_family == AF_INET6 && mask->sa_family == AF_INET6 && masked->sa_family == AF_INET6) {
        const size_t        off6 = offsetof(struct sockaddr_in6, sin6_addr);
        const unsigned char *a   = (const unsigned char *)addr   + off6;
        const unsigned char *m   = (const unsigned char *)mask   + off6;
        const unsigned char *o   = (const unsigned char *)masked + off6;

        /* (a & m) == o over 16 bytes */
        for (int i = 0; i < 16; i++) {
            if ((unsigned char)(a[i] & m[i]) != o[i]) {
                return NS_FALSE;
            }
        }
        return NS_TRUE;
    }


    /* IPv4 */
    if (addr->sa_family == AF_INET && mask->sa_family == AF_INET && masked->sa_family == AF_INET) {
        const size_t off4 = offsetof(struct sockaddr_in, sin_addr);
        uint32_t     a, m, o;  /* network byte order, bitwise AND is fine */

        memcpy(&a, (const unsigned char *)addr   + off4, 4);
        memcpy(&m, (const unsigned char *)mask   + off4, 4);
        memcpy(&o, (const unsigned char *)masked + off4, 4);

        return ((a & m) == o);
    }

    /*
     * Family mismatch.
     */
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrMaskBits --
 *
 *      Build a mask with the given bits in an IPv4 or IPv6 sockaddr
 *
 * Results:
 *      Mask computed in 1 arg.
 *
 * Side effects:
 *      The first argument is updated.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrMaskBits(const struct sockaddr *mask, unsigned int nrBits)
{
    unsigned char *dst;
    size_t         off;
    unsigned       full, rem;
    bool           success = NS_TRUE;

    NS_NONNULL_ASSERT(mask != NULL);

    if (mask->sa_family == AF_INET6) {
        /* cap /128 */
        if (nrBits > 128u) {
            Ns_Log(Warning, "Invalid bit mask /%u: at most 128 bits", nrBits);
            nrBits = 128u;
        }

        off = offsetof(struct sockaddr_in6, sin6_addr);
        dst = (unsigned char *)mask + off; /* 16 bytes */

        full = nrBits / 8u;     /* whole bytes set to 0xFF */
        rem  = nrBits % 8u;     /* high-bit count in next byte */

        memset(dst, 0xFF, full);
        if (full < 16u) {
            if (rem != 0u) {
                dst[full] = (unsigned char)(0xFFu << (8u - rem));
                memset(dst + full + 1u, 0x00, 16u - full - 1u);
            } else {
                memset(dst + full, 0x00, 16u - full);
            }
        }

    } else if (mask->sa_family == AF_INET) {
        /* cap /32 */
        if (nrBits > 32u) {
            Ns_Log(Warning, "Invalid bit mask /%u: at most 32 bits", nrBits);
            nrBits = 32u;
        }

        off = offsetof(struct sockaddr_in, sin_addr);
        dst = (unsigned char *)mask + off; /* 4 bytes */

        full = nrBits / 8u;
        rem  = nrBits % 8u;

        memset(dst, 0xFF, full);
        if (full < 4u) {
            if (rem != 0u) {
                dst[full] = (unsigned char)(0xFFu << (8u - rem));
                memset(dst + full + 1u, 0x00, 4u - full - 1u);
            } else {
                memset(dst + full, 0x00, 4u - full);
            }
        }

    } else {
        Ns_Log(Debug, "invalid address family %d detected (Ns_SockaddrMaskBits)", mask->sa_family);
        success = NS_FALSE;
    }
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrParseIPMask --
 *
 *      Build a mask and IPv4 or IpV6 address from an IP string notation,
 *      potentially containing a '/' for denoting the number of bits (CIDR
 *      notation)
 *
 *      Example: "137.208.1.10/16"
 *
 * Results:
 *      Binary IP address and mask are filled into last arguments,
 *      returns Ns_ReturnCode.
 *
 * Side effects:
 *      Memory pointed to by ipPtr and maskPtr is modified.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_SockaddrParseIPMask(Tcl_Interp *interp, const char *ipString,
                       struct sockaddr *ipPtr, struct sockaddr *maskPtr,
                       unsigned int *nrBitsPtr)
{
    char         *slash;
    int           validIP;
    unsigned int  nrBits = 0u;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(ipString != NULL);
    NS_NONNULL_ASSERT(ipPtr != NULL);
    NS_NONNULL_ASSERT(maskPtr != NULL);

    memset(ipPtr, 0, sizeof(struct NS_SOCKADDR_STORAGE));
    memset(maskPtr, 0, sizeof(struct NS_SOCKADDR_STORAGE));

    slash = strchr(ipString, INTCHAR('/'));

    if (slash == NULL) {
        /*
         * No mask is given
         */
        validIP = ns_inet_pton(ipPtr, ipString);
        if (validIP > 0) {
            maskPtr->sa_family = ipPtr->sa_family;
            nrBits = (maskPtr->sa_family == AF_INET6) ? 128 : 32;
            if (!Ns_SockaddrMaskBits(maskPtr, nrBits)) {
                status = NS_ERROR;
            }
        } else {
            status = NS_ERROR;
        }
    } else {
        int   validMask;
        char *dupIpString = ns_strdup(ipString);
        /*
         * Mask is given, try to convert the masked address into
         * binary values.
         */

        *(dupIpString + (slash-ipString)) = '\0';
        slash++;

        validIP = ns_inet_pton(ipPtr, dupIpString);
        if (strchr(slash, INTCHAR('.')) == NULL && strchr(slash, INTCHAR(':')) == NULL) {
            maskPtr->sa_family = ipPtr->sa_family;
            nrBits = (unsigned int)strtol(slash, NULL, 10);
            validMask = Ns_SockaddrMaskBits(maskPtr, nrBits);
        } else {
            nrBits = (maskPtr->sa_family == AF_INET6) ? 128 : 32;
            validMask = ns_inet_pton(maskPtr, slash);
        }

        if (validIP <= 0 || validMask < 0) {
            if (interp != NULL) {
                Ns_TclPrintfResult(interp, "invalid address or hostname \"%s\". "
                                   "Should be ipaddr/netmask or hostname", dupIpString);
            }
            status = NS_ERROR;
        }
        ns_free(dupIpString);
        /*
         * Do a bitwise AND of the IP address with the netmask
         * to make sure that all non-network bits are 0. That
         * saves us from doing this operation every time a
         * connection comes in.
         */
        if (!Ns_SockaddrMask(ipPtr, maskPtr, ipPtr)) {
            status = NS_ERROR;
        }
        /*Ns_LogSockaddr(Notice, "NSPERM: maskedAddress", ipPtr);*/
    }
    if (status == NS_OK && nrBitsPtr != NULL) {
        *nrBitsPtr = nrBits;
    }
    return status;
}



/*
 *----------------------------------------------------------------------
 *
 * ns_inet_ntop --
 *
 *    This function is a version of inet_ntop() which is agnostic to
 *    IPv4 and IPv6.
 *
 * Results:
 *    String pointing to printable IP address.
 *
 * Side effects:
 *    Update provided buffer with resulting character string.
 *
 *----------------------------------------------------------------------
 */
const char *
ns_inet_ntop(const struct sockaddr *NS_RESTRICT saPtr, char *NS_RESTRICT buffer, size_t size) {
    const char *result;

    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (saPtr->sa_family == AF_INET6) {
        const size_t off6 = offsetof(struct sockaddr_in6, sin6_addr);
        const void  *src6 = (const char *)saPtr + off6;

        result = inet_ntop(AF_INET6, src6, buffer, (socklen_t)size);
        if (result != NULL) {
            struct in6_addr a6;
            /*
             * In case the address is V4MAPPED, return just the IPv4 portion,
             * since ns_inet_pton() tries to return as well first an IPv4
             * address. This is important, since getsockname() might return
             * for a socket AF_INET6, although the socket was created with
             * AF_INET (see e.g. ListenCallback() in listen.c).
             *
             * Example of V4MAPPED address: ::ffff:127.0.0.1
             * Potential dangers:
             * https://tools.ietf.org/html/draft-itojun-v6ops-v4mapped-harmful-02
             */
            memcpy(&a6, src6, sizeof a6);

            if (IN6_IS_ADDR_V4MAPPED(&a6)) {
                /* Render the last 4 bytes as IPv4 */
                (void)inet_ntop(AF_INET, a6.s6_addr + 12, buffer, (socklen_t)size);
                /* Keep return value semantics: return buffer */
                result = buffer;
            }
        }

    } else {
        const size_t off4 = offsetof(struct sockaddr_in, sin_addr);
        const void  *src4 = (const char *)saPtr + off4;

        result = inet_ntop(AF_INET, src4, buffer, (socklen_t)size);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ns_inet_pton --
 *
 *  Convert an IPv4/IPv6 address in textual form to a binary IPv6
 *  form. IPV4 addresses are converted to "mapped IPv4 addresses".
 *
 * Results:
 *  >0  = Success.
 *  <=0 = Error:
 *   <0 = Invalid address family. As this routine hardcodes the AF,
 *        this result should not occur.
 *    0 = Parse error.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */
int
ns_inet_pton(struct sockaddr *saPtr, const char *addr) {
    int r;

    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(addr != NULL);


    /* Try IPv4 first */
    {
        const size_t off4 = offsetof(struct sockaddr_in, sin_addr);
        void        *dst4 = (void *)((char *)saPtr + off4);

        r = inet_pton(AF_INET, addr, dst4);
        if (r > 0) {
            saPtr->sa_family = AF_INET;
            return r;
        }
    }
#ifdef HAVE_IPV6
    /* Fallback to IPv6 */
    {
        const size_t off6 = offsetof(struct sockaddr_in6, sin6_addr);
        void        *dst6 = (void *)((char *)saPtr + off6);

        r = inet_pton(AF_INET6, addr, dst6);
        if (r > 0) {
            saPtr->sa_family = AF_INET6;
            return r;
        }
    }
#endif

    /* r is 0 (parse error) or -1 (errno set) from the last attempt */
    return r;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetSockAddr --
 *
 *      Take a host/port and fill in an NS_SOCKADDR_STORAGE structure
 *      appropriately. The passed in host may be an IP address or a DNS name.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      May perform DNS query.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_GetSockAddr(struct sockaddr *saPtr, const char *host, unsigned short port)
{
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(saPtr != NULL);

    /*
     * We return always a fresh sockaddr, so clear content first.
     */
    memset(saPtr, 0, sizeof(struct NS_SOCKADDR_STORAGE));

#ifdef HAVE_IPV6
    if (host == NULL) {
        saPtr->sa_family = AF_INET6;
        /* sin6_addr = in6addr_any (no typed pointer cast) */
        {
            const size_t off = offsetof(struct sockaddr_in6, sin6_addr);
            memcpy((char *)saPtr + off, &in6addr_any, sizeof(struct in6_addr));
        }
        /* sin6_port = htons(port) */
        {
            const size_t off = offsetof(struct sockaddr_in6, sin6_port);
            uint16_t p = htons(port);
            memcpy((char *)saPtr + off, &p, sizeof p);
        }

    } else {
        int r;

        r = ns_inet_pton((struct sockaddr *)saPtr, host);
        if (r <= 0) {
            Tcl_DString ds;

            Tcl_DStringInit(&ds);
            if (Ns_GetAddrByHost(&ds, host) == NS_TRUE) {
                r = ns_inet_pton((struct sockaddr *)saPtr, ds.string);
            }
            Tcl_DStringFree(&ds);
            if (r <= 0) {
                status = NS_ERROR;
            }
        }
    }

#else
    saPtr->sa_family = AF_INET;

    if (host == NULL) {
        ((struct sockaddr_in *)saPtr)->sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        ((struct sockaddr_in *)saPtr)->sin_addr.s_addr = inet_addr(host);
        if (((struct sockaddr_in *)saPtr)->sin_addr.s_addr == INADDR_NONE) {
            Tcl_DString ds;

            Tcl_DStringInit(&ds);
            if (Ns_GetAddrByHost(&ds, host) == NS_TRUE) {
                ((struct sockaddr_in *)saPtr)->sin_addr.s_addr = inet_addr(ds.string);
            }
            Tcl_DStringFree(&ds);
            if (((struct sockaddr_in *)saPtr)->sin_addr.s_addr == INADDR_NONE) {
                status = NS_ERROR;
            }
        }
    }
#endif
    if (likely(status == NS_OK)) {
        Ns_SockaddrSetPort((struct sockaddr *)saPtr, port);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrGetPort --
 *
 *      Generic function to obtain port from an IPv4 or IPv6 sock addr.
 *
 * Results:
 *      Port number
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
unsigned short
Ns_SockaddrGetPort(const struct sockaddr *saPtr)
{
    unsigned short port = 0;

    NS_NONNULL_ASSERT(saPtr != NULL);

#ifdef HAVE_IPV6
    if (saPtr->sa_family == AF_INET6) {
        const size_t off6 = offsetof(struct sockaddr_in6, sin6_port);
        memcpy(&port, (const char *)saPtr + off6, sizeof(port));
    } else
#endif
    {
        const size_t off4 = offsetof(struct sockaddr_in, sin_port);
        memcpy(&port, (const char *)saPtr + off4, sizeof(port));
    }

    return (unsigned short)ntohs(port);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrSetPort --
 *
 *      Generic function to set port in an IPv4 or IPv6 sock addr.
 *
 * Results:
 *      Port number
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
Ns_SockaddrSetPort(struct sockaddr *saPtr, unsigned short port)
{
    uint16_t netport = htons(port);

    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);

#ifdef HAVE_IPV6
    if (saPtr->sa_family == AF_INET6) {
        const size_t off6 = offsetof(struct sockaddr_in6, sin6_port);
        memcpy((char *)saPtr + off6, &netport, sizeof netport);
        return;
    }
#endif
    /* default / IPv4 */
    {
        const size_t off4 = offsetof(struct sockaddr_in, sin_port);
        memcpy((char *)saPtr + off4, &netport, sizeof netport);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrGetSockLen --
 *
 *      Generic function to obtain socklen from an IPv4 or IPv6 sockaddr.
 *
 * Results:
 *      socklen.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
socklen_t
Ns_SockaddrGetSockLen(const struct sockaddr *saPtr)
{
    size_t socklen;

    NS_NONNULL_ASSERT(saPtr != NULL);

#ifdef HAVE_IPV6
    socklen = (saPtr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
#else
    socklen = sizeof(struct sockaddr_in);
#endif

    return (socklen_t)socklen;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogSockSockaddr --
 *
 *      Function to log generic SockAddr.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Entry in log file.
 *
 *----------------------------------------------------------------------
 */
void
Ns_LogSockaddr(Ns_LogSeverity severity, const char *prefix, const struct sockaddr *saPtr)
{
    const char *family;
    char        ipString[NS_IPADDR_SIZE], *ipStrPtr = ipString;

    NS_NONNULL_ASSERT(prefix != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);

    family = (saPtr->sa_family == AF_INET6) ? "AF_INET6" :
        (saPtr->sa_family == AF_INET) ? "AF_INET" : "UNKNOWN";

    (void)ns_inet_ntop(saPtr, ipString, NS_IPADDR_SIZE);

    Ns_Log(severity, "%s: SockAddr family %s, ip %s, port %d",
           prefix, family, ipStrPtr, Ns_SockaddrGetPort(saPtr));
    /*fprintf(stderr, "%s: SockAddr family %s, ip %s, port %d\n",
      prefix, family, ipStrPtr, Ns_SockaddrGetPort(saPtr));*/
}


/*
 *----------------------------------------------------------------------
 *
 * SockAddrInit, SockkAddrInitMaskedEntry --
 *
 *      Initialization function for global data for efficient check of
 *      addresses and address ranges.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static void
SockkAddrInitMaskedEntry(const char *cdirString, MaskedEntry *entryPtr, const char *errorString)
{
    //fprintf(stderr, "SockkAddrInitMaskedEntry entryPtr %p cdir <%s>\n", (void*)entryPtr, cdirString);
    entryPtr->cdirString = ns_strdup(cdirString);
    if (Ns_SockaddrParseIPMask(NULL, entryPtr->cdirString,
                               (struct sockaddr *) &entryPtr->masked,
                               (struct sockaddr *) &entryPtr->mask,
                               NULL
                               ) != NS_OK) {
        Ns_Log(Error, "invalid CIDR %s during initialization: '%s'", errorString,  entryPtr->cdirString);
    }
}

static bool
SockAddrInit(void)
{
    size_t i;

    nonPublicEntries = ns_calloc(Ns_NrElements(nonPublicCIDR), sizeof(MaskedEntry));

    for (i = 0; i < Ns_NrElements(nonPublicCIDR) -1; i++) {
        SockkAddrInitMaskedEntry(nonPublicCIDR[i], &nonPublicEntries[i], "builtin value");
    }

    if (nsconf.reverseproxymode.trustedservers != NULL) {
        const char **elements;
        TCL_SIZE_T   length = 0;

        (void)Tcl_SplitList(NULL, nsconf.reverseproxymode.trustedservers, &length, &elements);
        if (length > 0) {
            size_t l = (size_t)length ++;

            trustedServersEntries = ns_calloc(l+1, sizeof(MaskedEntry));

            for (i = 0; i < l; i++) {
                SockkAddrInitMaskedEntry(elements[i], &trustedServersEntries[i], "value for reverseproxy");
            }
            Tcl_Free((char *) elements);
        }
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrTrustedReverseProxy --
 *
 *      Check, if the passed in socket address belongs to a trusted reverse
 *      proxy server.
 *
 * Results:
 *      Boolean.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrTrustedReverseProxy(const struct sockaddr *saPtr) {
    bool   success = NS_FALSE;
    size_t i;

    NS_NONNULL_ASSERT(saPtr != NULL);

    NS_INIT_ONCE(SockAddrInit);

    if (trustedServersEntries != NULL) {
        for (i = 0u; trustedServersEntries[i].cdirString != NULL; i++) {
            //Ns_Log(Notice, "[%ld] trusted reverse proxy check %p> ", i, (void*)trustedServersEntries[i].cdirString) ;
            //Ns_Log(Notice, "[%ld] trusted reverse proxy check <%s> ", i, trustedServersEntries[i].cdirString);
            if (Ns_SockaddrMaskedMatch(saPtr,
                                       (struct sockaddr *) &trustedServersEntries[i].mask,
                                       (struct sockaddr *) &trustedServersEntries[i].masked)) {
                success = NS_TRUE;
                break;
            }
        }
    }
#if 0
    {
        char   ipString[NS_IPADDR_SIZE];
        size_t j;
        for (j = 0u; trustedServersEntries[j].cdirString != NULL; j++) {}
        (void)ns_inet_ntop(saPtr, ipString, NS_IPADDR_SIZE);
        Ns_Log(Notice, "...... checked %ld/%ld trusted %s -> %d", i, j, ipString, success);
    }
#endif
    return success;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrPublicIpAddress --
 *
 *      Check, if the passed in socket address is a public (non-local and
 *      routable) IP address.
 *
 * Results:
 *      Boolean.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrPublicIpAddress(const struct sockaddr *saPtr) {
    bool   success = NS_TRUE;
    size_t i;

    NS_NONNULL_ASSERT(saPtr != NULL);

    NS_INIT_ONCE(SockAddrInit);

    for (i = 0u; nonPublicCIDR[i] != NULL; i++) {
        //Ns_Log(Notice, "public IP check <%s> ", nonPublicCIDR[i]);

        if (Ns_SockaddrMaskedMatch(saPtr,
                                   (struct sockaddr *) &nonPublicEntries[i].mask,
                                   (struct sockaddr *) &nonPublicEntries[i].masked)) {
            success = NS_FALSE;
            break;
        }
    }
#if 0
    {
        char   ipString[NS_IPADDR_SIZE];
        size_t j;
        for (j = 0u; nonPublicCIDR[j] != NULL; j++) {}
        (void)ns_inet_ntop(saPtr, ipString, NS_IPADDR_SIZE);
        Ns_Log(Notice, "...... checked %ld/%ld public %s -> %d", i, j, ipString, success);
    }
#endif
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrInAny --
 *
 *      Determines whether the given socket address represents the
 *      "any" (unspecified) address. For an IPv4 address, this is equivalent to
 *      INADDR_ANY (usually 0.0.0.0), and for an IPv6 address, it is equivalent
 *      to the in6addr_any (an all-zero address). The function returns NS_TRUE
 *      if the address is unspecified, and NS_FALSE otherwise.
 *
 * Returns:
 *      NS_TRUE  if the address is the "any" address (i.e., unspecified).
 *      NS_FALSE otherwise, including when the address family is neither AF_INET
 *               nor AF_INET6.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrInAny(const struct sockaddr *saPtr) {
    bool success = NS_TRUE;

    NS_NONNULL_ASSERT(saPtr != NULL);

    switch (saPtr->sa_family) {
        case AF_INET: {
            const size_t off4 = offsetof(struct sockaddr_in, sin_addr);
            struct in_addr a4;

            memcpy(&a4, (const char *)saPtr + off4, sizeof a4);
            success = (a4.s_addr == htonl(INADDR_ANY));
            break;
        }
        case AF_INET6: {
            const size_t off6 = offsetof(struct sockaddr_in6, sin6_addr);
            struct in6_addr a6;
            memcpy(&a6, (const char *)saPtr + off6, sizeof a6);
            success = (memcmp(&a6, &in6addr_any, sizeof a6) == 0);
            break;
        }
        default: {
            /*
             * Not IPv4 or IPv6
             */
            success = NS_FALSE;
            break;
        }
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrSetLoopback --
 *
 *      Set the IP address of a given sockaddr structure to the loopback
 *      address, depending on its address family.
 *
 *      - For AF_INET  (IPv4): sets sin_addr.s_addr to 127.0.0.1 (INADDR_LOOPBACK).
 *      - For AF_INET6 (IPv6): sets sin6_addr to the standard IPv6 loopback (::1).
 *      - For other address families: does nothing and reports failure.
 *
 * Returns:
 *      NS_TRUE  if the address family is AF_INET or AF_INET6 and the loopback
 *               address was successfully applied.
 *      NS_FALSE if the address family is unsupported and no changes were made.
 *
 * Side Effects:
 *      Modifies the input sockaddr structure in place.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrSetLoopback(struct sockaddr *saPtr) {
    bool success = NS_TRUE;

    NS_NONNULL_ASSERT(saPtr != NULL);

    switch (saPtr->sa_family) {
    case AF_INET: {
        const size_t   off = offsetof(struct sockaddr_in, sin_addr);
        struct in_addr a4;
        a4.s_addr = htonl(INADDR_LOOPBACK);                 /* 127.0.0.1 in network order */
        memcpy((char *)saPtr + off, &a4, sizeof a4);
        break;
    }
    case AF_INET6: {
        const size_t off = offsetof(struct sockaddr_in6, sin6_addr);
        memcpy((char *)saPtr + off, &in6addr_loopback, sizeof(struct in6_addr)); /* ::1 */
        break;
    }
    default:
        /*
         * Not IPv4 or IPv6
         */
        success = NS_FALSE;
        break;
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrAddToDictIpProperties --
 *
 *      Add for the speicied IP address properties to the provided dict.
 *
 * Results:
 *      TCL_OK;
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
int
Ns_SockaddrAddToDictIpProperties(const struct sockaddr *ipPtr, Tcl_Obj *dictObj) {
    bool     isPublic = Ns_SockaddrPublicIpAddress(ipPtr);
    bool     isTrusted = Ns_SockaddrTrustedReverseProxy(ipPtr);
    bool     isInAny = Ns_SockaddrInAny(ipPtr);
    Tcl_Obj *typeValueObj;

    Tcl_DictObjPut(NULL, dictObj,
                   Tcl_NewStringObj("public", 6),
                   Tcl_NewBooleanObj(isPublic));
    Tcl_DictObjPut(NULL, dictObj,
                   Tcl_NewStringObj("trusted", 7),
                   Tcl_NewBooleanObj(isTrusted));
    Tcl_DictObjPut(NULL, dictObj,
                   Tcl_NewStringObj("inany", 5),
                   Tcl_NewBooleanObj(isInAny));
    if (ipPtr->sa_family == AF_INET) {
        typeValueObj = Tcl_NewStringObj("IPv4", 4);
    } else if (ipPtr->sa_family == AF_INET6) {
        typeValueObj = Tcl_NewStringObj("IPv6", 4);
    } else {
        typeValueObj = Tcl_NewStringObj("unknown", 7);
    }
    Tcl_DictObjPut(NULL, dictObj,
                   Tcl_NewStringObj("type", 4),
                   typeValueObj);
    return TCL_OK;
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
