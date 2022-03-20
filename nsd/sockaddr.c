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
void
Ns_SockaddrMask(const struct sockaddr *addr, const struct sockaddr *mask, struct sockaddr *maskedAddr)
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
          fprintf(stderr, "#### addr   %s\n", ns_inet_ntoa(addr));
          fprintf(stderr, "#### mask   %s\n", ns_inet_ntoa(mask));
          fprintf(stderr, "#### masked %s\n", ns_inet_ntoa(maskedAddr));
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
    bool success;

    NS_NONNULL_ASSERT(addr1 != NULL);
    NS_NONNULL_ASSERT(addr2 != NULL);

    if (addr1 == addr2) {
        success = NS_TRUE;

    } else if (addr1->sa_family == AF_INET6 && addr2->sa_family == AF_INET6) {
        const struct in6_addr *addr1Bits  = &(((struct sockaddr_in6 *)addr1)->sin6_addr);
        const struct in6_addr *addr2Bits  = &(((struct sockaddr_in6 *)addr2)->sin6_addr);
        int i;

        success = NS_TRUE;
        /*
         * Perform bitwise comparison. Maybe something special is needed for
         * comparing IPv4 address with IN6_IS_ADDR_V4MAPPED
         */
#ifndef _WIN32
        for (i = 0; i < 4; i++) {
            if (addr1Bits->s6_addr32[i] != addr2Bits->s6_addr32[i]) {
                success = NS_FALSE;
                break;
            }
        }
#else
        for (i = 0; i < 8; i++) {
            if (addr1Bits->u.Word[i] != addr2Bits->u.Word[i]) {
                success = NS_FALSE;
                break;
            }
        }
#endif
    } else if (addr1->sa_family == AF_INET && addr2->sa_family == AF_INET) {
        success = (((struct sockaddr_in *)addr1)->sin_addr.s_addr
                  == ((struct sockaddr_in *)addr2)->sin_addr.s_addr);
    } else {
        /*
         * Family mismatch.
         */
        success = NS_FALSE;
    }

    return success;
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
    bool success;

    NS_NONNULL_ASSERT(addr != NULL);
    NS_NONNULL_ASSERT(mask != NULL);
    NS_NONNULL_ASSERT(masked != NULL);

    if (addr == mask) {
        success = NS_TRUE;

    } else if (addr->sa_family == AF_INET6 && mask->sa_family == AF_INET6) {
        const struct in6_addr *addrBits   = &(((struct sockaddr_in6 *)addr)->sin6_addr);
        const struct in6_addr *maskBits   = &(((struct sockaddr_in6 *)mask)->sin6_addr);
        const struct in6_addr *maskedBits = &(((struct sockaddr_in6 *)masked)->sin6_addr);

        int i;

        success = NS_TRUE;
        /*
         * Perform bitwise comparison. Maybe something special is needed for
         * comparing IPv4 address with IN6_IS_ADDR_V4MAPPED
         */
#ifndef _WIN32
        for (i = 0; i < 4; i++) {
            if ((addrBits->s6_addr32[i] & maskBits->s6_addr32[i]) != maskedBits->s6_addr32[i]) {
                success = NS_FALSE;
                break;
            }
        }
#else
        for (i = 0; i < 8; i++) {
            if ((addrBits->u.Word[i] & maskBits->u.Word[i]) != maskedBits->u.Word[i]) {
                success = NS_FALSE;
                break;
            }
        }
#endif
    } else if (addr->sa_family == AF_INET && mask->sa_family == AF_INET) {
        /* fprintf(stderr, "addr %.8x & mask %.8x masked %.8x <-> %.8x\n",
                ((struct sockaddr_in *)addr)->sin_addr.s_addr,
                ((struct sockaddr_in *)mask)->sin_addr.s_addr,
                ((struct sockaddr_in *)masked)->sin_addr.s_addr,
                (((struct sockaddr_in *)addr)->sin_addr.s_addr
                & ((struct sockaddr_in *)mask)->sin_addr.s_addr));*/
        success = ((((struct sockaddr_in *)addr)->sin_addr.s_addr
                    & ((struct sockaddr_in *)mask)->sin_addr.s_addr) ==
                   (((struct sockaddr_in *)masked)->sin_addr.s_addr));
    } else {
        /*
         * Family mismatch.
         */
        success = NS_FALSE;
    }

    return success;
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
void
Ns_SockaddrMaskBits(const struct sockaddr *mask, unsigned int nrBits)
{
    NS_NONNULL_ASSERT(mask != NULL);

    if (mask->sa_family == AF_INET6) {
        struct in6_addr *addr = &(((struct sockaddr_in6 *)mask)->sin6_addr);
        int i;

        if (nrBits > 128u) {
            Ns_Log(Warning, "Invalid bit mask /%d: can be most 128 bits", nrBits);
            nrBits = 128u;
        }
#ifndef _WIN32
        /*
         * Set the mask bits in the leading 32 bit ints to 1.
         */
        for (i = 0; i < 4 && nrBits >= 32u; i++, nrBits -= 32u) {
            addr->s6_addr32[i] = (~0u);
        }
        /*
         * Set the partial mask.
         */
        if (i < 4 && nrBits > 0u) {
            addr->s6_addr32[i] = htonl((~0u) << (32u - nrBits));
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
         * Windows does not have 32-bit members, so process in 16-bit
         * chunks: Set the mask bits in the leading 16 bit Words to 1.
         */
        for (i = 0; i < 8 && nrBits >= 16u; i++, nrBits -= 16u) {
            addr->u.Word[i] = (unsigned short)(~0u);
        }
        /*
         * Set the partial mask.
         */
        if (i < 8 && nrBits > 0u) {
            addr->u.Word[i] = htons((~0u) << (16u - nrBits));
            i++;
        }
        /*
         * Clear trailing 16 bit Words.
         */
        for (; i < 8; i++) {
            addr->u.Word[i] = 0u;
        }
#endif
        /*fprintf(stderr, "#### FINAL mask %s\n", ns_inet_ntoa(mask));*/
    } else if (mask->sa_family == AF_INET) {
        if (nrBits > 32u) {
            Ns_Log(Warning, "Invalid bit mask /%d: can be most 32 bits", nrBits);
            nrBits = 32u;
        }
        ((struct sockaddr_in *)mask)->sin_addr.s_addr = htonl((~0u) << (32u - nrBits));
    } else {
        Ns_Log(Error, "invalid address family %d detected (Ns_SockaddrMaskBits)", mask->sa_family);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockaddrParseIPMask --
 *
 *      Build a mask and IPv4 or IpV6 address from an IP string notation,
 *      potentially containing a '/' for denoting the number of bits.
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
            Ns_SockaddrMaskBits(maskPtr, nrBits);
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
            Ns_SockaddrMaskBits(maskPtr, nrBits);
            validMask = 1;
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
        Ns_SockaddrMask(ipPtr, maskPtr, ipPtr);
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
        result = inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)saPtr)->sin6_addr,
                           buffer, (socklen_t)size);

        if (result != NULL) {
            const struct in6_addr *addr = &(((struct sockaddr_in6 *)saPtr)->sin6_addr);
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
            if (IN6_IS_ADDR_V4MAPPED(addr)) {
                const char *tail = strrchr(result, INTCHAR(':'));

                /*
                 * When the last ':' in the converted string is further away
                 * from the end as possible with a pure IPv6 notation, then
                 * assume the last portion is an IPv4 address.
                 */
                if (tail != NULL) {
                    size_t len = strlen(tail);

                    if (len > 6 && len < size) {
                        tail ++;
                        memcpy(buffer, tail, len);
                        buffer[len] = '\0';
                    }
                }
            }
        }
    } else {
        result = inet_ntop(AF_INET, &((const struct sockaddr_in *)saPtr)->sin_addr,
                           buffer, (socklen_t)size);
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
            Ns_DString ds;

            Ns_DStringInit(&ds);
            if (Ns_GetAddrByHost(&ds, host) == NS_TRUE) {
                ((struct sockaddr_in *)saPtr)->sin_addr.s_addr = inet_addr(ds.string);
            }
            Ns_DStringFree(&ds);
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
    unsigned short port;

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

    return (unsigned short)htons(port);
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
