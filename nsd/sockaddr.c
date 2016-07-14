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

#include "nsd.h"

/*
 * sockaddr.c --
 *
 *      Generic Interface for IPv4 and IPv6
 */



/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrMask --
 *
 *	Compute from and address and a mask a masked address in a generic way
 *	(for IPv4 and IPv6 addresses).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The last argument (maskedAddr) is updated.
 *
 *----------------------------------------------------------------------
 */
void
Ns_SockaddrMask(struct sockaddr *addr, struct sockaddr *mask, struct sockaddr *maskedAddr)
{
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
        const struct in6_addr *addrBits   = &(((struct sockaddr_in6 *)addr)->sin6_addr);
        const struct in6_addr *maskBits   = &(((struct sockaddr_in6 *)mask)->sin6_addr);
        struct in6_addr       *maskedBits = &(((struct sockaddr_in6 *)maskedAddr)->sin6_addr);
        int i;

        /*
         * Perform bitwise masking over the full array. Maybe we need
         * something special for IN6_IS_ADDR_V4MAPPED.
         */
#ifndef _WIN32
        for (i = 0; i < 4; i++) {
            maskedBits->s6_addr32[i] = addrBits->s6_addr32[i] & maskBits->s6_addr32[i];
        }
#else        
        for (i = 0; i < 8; i++) {
            maskedBits->u.Word[i] = addrBits->u.Word[i] & maskBits->u.Word[i];
        }
#endif        
        /*
          fprintf(stderr, "#### addr   %s\n",ns_inet_ntoa(addr));
          fprintf(stderr, "#### mask   %s\n",ns_inet_ntoa(mask));
          fprintf(stderr, "#### masked %s\n",ns_inet_ntoa(maskedAddr));
        */
    } else if (addr->sa_family == AF_INET && mask->sa_family == AF_INET) {
        ((struct sockaddr_in *)maskedAddr)->sin_addr.s_addr =
            ((struct sockaddr_in *)addr)->sin_addr.s_addr &
            ((struct sockaddr_in *)mask)->sin_addr.s_addr;
    } else if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
        Ns_Log(Error, "nsperm: invalid address family %d detected (Ns_SockaddrMask addr)", addr->sa_family);
    } else if (mask->sa_family != AF_INET && mask->sa_family != AF_INET6) {
        Ns_Log(Error, "nsperm: invalid address family %d detected (Ns_SockaddrMask mask)", mask->sa_family);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrSameIP --
 *
 *	Check, if to sockaddrs refer to the same IP address
 *	(for IPv4 and IPv6 addresses).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockaddrSameIP(struct sockaddr *addr1, struct sockaddr *addr2)
{
    NS_NONNULL_ASSERT(addr1 != NULL);
    NS_NONNULL_ASSERT(addr2 != NULL);

    if (addr1 == addr2) {
        return NS_TRUE;
    }
    
    if (addr1->sa_family == AF_INET6 && addr2->sa_family == AF_INET6) {
        const struct in6_addr *addr1Bits  = &(((struct sockaddr_in6 *)addr1)->sin6_addr);
        const struct in6_addr *addr2Bits  = &(((struct sockaddr_in6 *)addr2)->sin6_addr);
        int i;
        
        /*
         * Perform bitwise comparison. Maybe something special is needed for
         * comparing IPv4 address with IN6_IS_ADDR_V4MAPPED
         */

#ifndef _WIN32
        for (i = 0; i < 4; i++) {
            if (addr1Bits->s6_addr32[i] != addr2Bits->s6_addr32[i]) {
                return NS_FALSE;
            }
        }
#else        
        for (i = 0; i < 8; i++) {
            if (addr1Bits->u.Word[i] != addr2Bits->u.Word[i]) {
                return NS_FALSE;
            }
        }
#endif
    } else if (addr1->sa_family == AF_INET && addr2->sa_family == AF_INET) {
        return (((struct sockaddr_in *)addr1)->sin_addr.s_addr
                == ((struct sockaddr_in *)addr2)->sin_addr.s_addr);
    } else {
        /*
         * Family mismatch.
         */
        return NS_FALSE;
    }
    
    return NS_TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrMaskBits --
 *
 *	Build a mask with the given bits in a IPv4 or IPv6 sockaddr
 *
 * Results:
 *	Mask computed in 1 arg.
 *
 * Side effects:
 *	The first argument is updated.
 *
 *----------------------------------------------------------------------
 */
void
Ns_SockaddrMaskBits(struct sockaddr *mask, unsigned int nrBits)
{
    NS_NONNULL_ASSERT(mask != NULL);

    if (mask->sa_family == AF_INET6) {
        struct in6_addr *addr = &(((struct sockaddr_in6 *)mask)->sin6_addr);
        int i;

        if (nrBits > 128u) {
            Ns_Log(Warning, "Invalid bitmask /%d: can be most 128 bits", nrBits);
            nrBits = 128u;
        }
#ifndef _WIN32        
        /*
         * Set the mask bits in the leading 32 bit ints to 1.
         */
        for (i = 0; i < 4 && nrBits >= 32u; i++, nrBits -= 32) {
            addr->s6_addr32[i] = (~0u);
        }
        /*
         * Set the partial mask.
         */
        if (i < 4 && nrBits > 0u) {
            addr->s6_addr32[i] = htonl((~0u) << (32 - nrBits));
            i++;
        }
        /*
         * Clear trailing 32 bit ints.
         */
        for (; i < 4; i++) {
            addr->s6_addr32[i] = 0u;
        }
#else
        /*
         * Windows does not have 32bit members, so process in 16bit
         * chunks: Set the mask bits in the leading 16 bit Words to 1.
         */
        for (i = 0; i < 8 && nrBits >= 16u; i++, nrBits -= 16) {
            addr->u.Word[i] = (unsigned short)(~0u);
        }
        /*
         * Set the partial mask.
         */
        if (i < 8 && nrBits > 0u) {
            addr->u.Word[i] = htons((~0u) << (16 - nrBits));
            i++;
        }
        /*
         * Clear trailing 16 bit Words.
         */
        for (; i < 8; i++) {
            addr->u.Word[i] = 0u;
        }        
#endif        
        /*fprintf(stderr, "#### FINAL mask %s\n",ns_inet_ntoa(mask));*/
    } else if (mask->sa_family == AF_INET) {
        if (nrBits > 32u) {
            Ns_Log(Warning, "Invalid bitmask /%d: can be most 32 bits", nrBits);
            nrBits = 32u;
        }
        ((struct sockaddr_in *)mask)->sin_addr.s_addr = htonl((~0u) << (32 - nrBits));
    } else {
        Ns_Log(Error, "invalid address family %d detected (Ns_SockaddrMaskBits)", mask->sa_family);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ns_inet_ntop --
 *
 *    This function is a version of inet_ntop() which is agnostic to IPv4 and IPv6.
 *
 * Results:
 *    String pointing to printable ip address.
 *
 * Side effects:
 *    Update provided buffer with resulting character string.
 *
 *----------------------------------------------------------------------
 */
const char *
ns_inet_ntop(const struct sockaddr *saPtr, char *buffer, size_t size) {
    const char *result;

    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (saPtr->sa_family == AF_INET6) {
        result = inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)saPtr)->sin6_addr, buffer, size);
    } else {
        result = inet_ntop(AF_INET, &((const struct sockaddr_in *)saPtr)->sin_addr, buffer, size);
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

    /*
     * First try whether the address parses as an IPv4 address
     */
    r = inet_pton(AF_INET, addr, &((struct sockaddr_in *)saPtr)->sin_addr);
    if (r > 0) {
        saPtr->sa_family = AF_INET;
        /*Ns_LogSockaddr(Notice, "ns_inet_pton returns IPv4 address", saPtr);*/
    } else {
#ifdef HAVE_IPV6
        /*
         * No IPv4 address, try to parse as IPv6 address
         */
        r = inet_pton(AF_INET6, addr, &((struct sockaddr_in6 *)saPtr)->sin6_addr);
        saPtr->sa_family = AF_INET6;

        /*Ns_LogSockaddr(Notice, "ns_inet_pton returns IPv6 address", saPtr);*/
#endif
    }
    return r;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetSockAddr --
 *
 *      Take a host/port and fill in a NS_SOCKADDR_STORAGE structure
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
Ns_GetSockAddr(struct sockaddr *saPtr, const char *host, int port)
{
    NS_NONNULL_ASSERT(saPtr != NULL);

    /*
     * We return always a fresh sockaddr, so clear content first.
     */
    memset(saPtr, 0, sizeof(struct NS_SOCKADDR_STORAGE));
    
#ifdef HAVE_IPV6
    if (host == NULL) {
        saPtr->sa_family = AF_INET6;
        ((struct sockaddr_in6 *)saPtr)->sin6_addr = in6addr_any;
    } else {
        int r;

        r = ns_inet_pton((struct sockaddr *)saPtr, host);
        if (r <= 0) {
            Ns_DString ds;

            Ns_DStringInit(&ds);
            if (Ns_GetAddrByHost(&ds, host) == NS_TRUE) {
                r = ns_inet_pton((struct sockaddr *)saPtr, ds.string);
            }
            Ns_DStringFree(&ds);
            if (r <= 0) {
                return NS_ERROR;
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
            Ns_DString ds;

            Ns_DStringInit(&ds);
            if (Ns_GetAddrByHost(&ds, host) == NS_TRUE) {
                ((struct sockaddr_in *)saPtr)->sin_addr.s_addr = inet_addr(ds.string);
            }
            Ns_DStringFree(&ds);
            if (((struct sockaddr_in *)saPtr)->sin_addr.s_addr == INADDR_NONE) {
                return NS_ERROR;
            }
        }
    }
#endif

    Ns_SockaddrSetPort((struct sockaddr *)saPtr, port);

    return NS_OK;
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
    unsigned int port;
    
    NS_NONNULL_ASSERT(saPtr != NULL);
    
#ifdef HAVE_IPV6
    if (saPtr->sa_family == AF_INET6) {
        port = ((const struct sockaddr_in6 *)saPtr)->sin6_port;
    } else {
        port = ((const struct sockaddr_in *)saPtr)->sin_port;
    }
#else
    port = ((const struct sockaddr_in *)saPtr)->sin_port;
#endif
    
    return htons(port);
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
    NS_NONNULL_ASSERT(saPtr != NULL);
    
#ifdef HAVE_IPV6
    if (saPtr->sa_family == AF_INET6) {
        ((struct sockaddr_in6 *)saPtr)->sin6_port = ntohs(port);
    } else {
        ((struct sockaddr_in *)saPtr)->sin_port = ntohs(port);
    }
#else
    ((struct sockaddr_in *)saPtr)->sin_port = ntohs(port);
#endif

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
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
