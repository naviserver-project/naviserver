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
 * binder.c --
 *
 * Support for pre-bound privileged ports.
 */

#include "nsd.h"

#ifndef _WIN32
# include <sys/un.h>
#endif

NS_RCSID("@(#) $Header$");

/*
 * Local variables defined in this file
 */

static Tcl_HashTable preboundTcp;
static Tcl_HashTable preboundUdp;
static Tcl_HashTable preboundRaw;
static Tcl_HashTable preboundUnix;
static Ns_Mutex lock;

/*
 * Local functions defined in this file
 */

static void PreBind(char *line);


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenEx --
 *
 *      Create a new TCP socket bound to the specified port and
 *      listening for new connections.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockListenEx(char *address, int port, int backlog)
{
    int sock = -1;
    struct sockaddr_in sa;

    if (Ns_GetSockAddr(&sa, address, port) == NS_OK) {
        Tcl_HashEntry *hPtr;
        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(&preboundTcp, (char *) &sa);
        if (hPtr != NULL) {
            sock = (int) Tcl_GetHashValue(hPtr);
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&lock);
        if (hPtr == NULL) {
            /* Not prebound, bind now */
            sock = Ns_SockBind(&sa);
        }
        if (sock >= 0 && listen(sock, backlog) == -1) {
            /* Can't listen; close the opened socket */
            int err = errno;
            close(sock);
            errno = err;
            sock = -1;
            Ns_SetSockErrno(err);
        }
    }

    return (SOCKET)sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenUdp --
 *
 *      Listen on the UDP socket for the given IP address and port.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      May create a new socket if none prebound.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockListenUdp(char *address, int port)
{
    int sock = -1;
    struct sockaddr_in sa;

    if (Ns_GetSockAddr(&sa, address, port) == NS_OK) {
        Tcl_HashEntry *hPtr;
        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(&preboundUdp, (char *) &sa);
        if (hPtr != NULL) {
            sock = (int) Tcl_GetHashValue(hPtr);
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&lock);
        if (hPtr == NULL) {
            /* Not prebound, bind now */
            sock = Ns_SockBindUdp(&sa);
        }
    }

    return (SOCKET)sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenRaw --
 *
 *      Listen on the raw socket addressed by the given protocol.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      May create a new socket if none prebound.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockListenRaw(int proto)
{
    int sock = -1;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    Ns_MutexLock(&lock);
    hPtr = Tcl_FirstHashEntry(&preboundRaw, &search);
    while (hPtr != NULL) {
        if (proto == (int)Tcl_GetHashValue(hPtr)) {
            sock = (int)Tcl_GetHashKey(&preboundRaw, hPtr);
            Tcl_DeleteHashEntry(hPtr);
            break;
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&lock);
    if (hPtr == NULL) {
        /* Not prebound, bind now */
        sock = Ns_SockBindRaw(proto);
    }

    return (SOCKET)sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenUnix --
 *
 *      Listen on the Unix-domain socket addressed by the given path.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      May create a new socket if none prebound.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockListenUnix(char *path, int backlog)
{
    int sock = -1;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

#ifndef _WIN32
    Ns_MutexLock(&lock);
    hPtr = Tcl_FirstHashEntry(&preboundUnix, &search);
    while (hPtr != NULL) {
        if (!strcmp(path, (char*)Tcl_GetHashValue(hPtr))) {
            sock = (int)Tcl_GetHashKey(&preboundRaw, hPtr);
            Tcl_DeleteHashEntry(hPtr);
            break;
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&lock);
    if (hPtr == NULL) {
        /* Not prebound, bind now */
        sock = Ns_SockBindUnix(path);
    }
    if (sock >= 0 && listen(sock, backlog) == -1) {
        /* Can't listen; close the opened socket */
        int err = errno;
        close(sock);
        errno = err;
        sock = -1;
        Ns_SetSockErrno(err);
    }
#endif

    return (SOCKET) sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBindUdp --
 *
 *      Create a UDP socket and bind it to the passed-in address.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockBindUdp(struct sockaddr_in *saPtr)
{
   int sock = -1, n = 1;
   
   sock = socket(AF_INET,SOCK_DGRAM, 0);

   if (sock == -1
       || setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&n,sizeof(n)) == -1
       || bind(sock,(struct sockaddr*)saPtr,sizeof(struct sockaddr_in)) == -1) {
       int err = errno;
       close(sock);
       sock = -1;
       Ns_SetSockErrno(err);
   }

   return (SOCKET)sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBindUnix --
 *
 *      Create a Unix-domain socket and bind it to the passed-in
 *      file path.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockBindUnix(char *path)
{
    int sock = -1;
    struct sockaddr_un addr;

#ifndef _WIN32    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path,path, sizeof(addr.sun_path) - 1);
    unlink(path);
    
    sock = socket(AF_UNIX,SOCK_STREAM, 0);
    
    if (sock == -1
        || bind(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        int err = errno;
        close(sock);
        sock = -1;
        Ns_SetSockErrno(err);
    }
#endif
    
    return (SOCKET) sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBindRaw --
 *
 *      Create a raw socket. It does not bind, hence the call name
 *      is not entirely correct but is on-pair with other types of
 *      sockets (udp, tcp, unix).
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockBindRaw(int proto)
{
   int sock = -1;
   
   sock = socket(AF_INET,SOCK_RAW, proto);

   if (sock == -1) {
       int err = errno;
       close(sock);
       Ns_SetSockErrno(err);
   }

   return (SOCKET)sock;
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitBinder --
 *
 *      Initialize the pre-bind tables.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsInitBinder(void)
{
    Tcl_InitHashTable(&preboundTcp, sizeof(struct sockaddr_in)/sizeof(int));
    Tcl_InitHashTable(&preboundUdp, sizeof(struct sockaddr_in)/sizeof(int));
    Tcl_InitHashTable(&preboundRaw, TCL_ONE_WORD_KEYS);
    Tcl_InitHashTable(&preboundUnix, TCL_STRING_KEYS);
}


/*
 *----------------------------------------------------------------------
 *
 * NsPreBind --
 *
 *      Pre-bind any requested ports (called from Ns_Main at startup).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May pre-bind to one or more ports.
 *
 *----------------------------------------------------------------------
 */

void
NsPreBind(char *args, char *file)
{
#ifndef _WIN32
    if (args != NULL) {
        PreBind(args);
    }
    if (file != NULL) {
        Tcl_Channel chan = Tcl_OpenFileChannel(NULL, file, "r", 0);
        if (chan == NULL) {
            Ns_Log(Error, "binder: can't open file '%s': '%s'", file,
                   strerror(Tcl_GetErrno()));
        } else {
            Tcl_DString line;
            Tcl_DStringInit(&line);
            while(!Tcl_Eof(chan)) {
                Tcl_DStringSetLength(&line, 0);
                if (Tcl_Gets(chan, &line) > 0) {
                    PreBind(Tcl_DStringValue(&line));
                }
            }
            Tcl_DStringFree(&line);
            Tcl_Close(NULL, chan);
        }
    }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * NsClosePreBound --
 *
 *      Close remaining pre-bound sockets not consumed by anybody.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Pre-bind hash-tables are cleaned and re-initialized.
 *
 *----------------------------------------------------------------------
 */

void
NsClosePreBound(void)
{
#ifdef _WIN32
    return;
#else
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    char *addr;
    int port, sock;
    struct sockaddr_in *saPtr;
    
    Ns_MutexLock(&lock);

    /*
     * Close TCP sockets
     */

    hPtr = Tcl_FirstHashEntry(&preboundTcp, &search);
    while (hPtr != NULL) {
        saPtr = (struct sockaddr_in *) Tcl_GetHashKey(&preboundTcp, hPtr);
        addr = ns_inet_ntoa(saPtr->sin_addr);
        port = htons(saPtr->sin_port);
        sock = (int)Tcl_GetHashValue(hPtr);
        Ns_Log(Warning, "prebind: closed unused TCP socket: %s:%d = %d", 
               addr, port, sock);
        close(sock);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundTcp);
    Tcl_InitHashTable(&preboundTcp, sizeof(struct sockaddr_in)/sizeof(int));

    /*
     * Close UDP sockets
     */

    hPtr = Tcl_FirstHashEntry(&preboundUdp, &search);
    while (hPtr != NULL) {
        saPtr = (struct sockaddr_in *) Tcl_GetHashKey(&preboundUdp, hPtr);
        addr = ns_inet_ntoa(saPtr->sin_addr);
        port = htons(saPtr->sin_port);
        sock = (int)Tcl_GetHashValue(hPtr);
        Ns_Log(Warning, "prebind: closed unused UDP socket: %s:%d = %d", 
               addr, port, sock);
        close(sock);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundUdp);
    Tcl_InitHashTable(&preboundUdp, sizeof(struct sockaddr_in)/sizeof(int));

    /*
     * Close raw sockets
     */

    hPtr = Tcl_FirstHashEntry(&preboundRaw, &search);
    while (hPtr != NULL) {
        sock = (int)Tcl_GetHashKey(&preboundRaw, hPtr);
        port = (int)Tcl_GetHashValue(hPtr);
        Ns_Log(Warning, "prebind: closed unused raw socket: %d = %d", 
               port, sock);
        close(sock);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundRaw);
    Tcl_InitHashTable(&preboundRaw, TCL_ONE_WORD_KEYS);

    /*
     * Close Unix-domain sockets
     */

    hPtr = Tcl_FirstHashEntry(&preboundUnix, &search);
    while (hPtr != NULL) {
        addr = (char *) Tcl_GetHashKey(&preboundUnix, hPtr);
        sock = (int)Tcl_GetHashValue(hPtr);
        Ns_Log(Warning, "prebind: closed unused Unix-domain socket: %s = %d",
               addr, sock);
        close(sock);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundUnix);
    Tcl_InitHashTable(&preboundUnix, TCL_STRING_KEYS);

    Ns_MutexUnlock(&lock);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * PreBind --
 *
 *      Pre-bind to one or more ports in a comma-separated list:
 *
 *          addr:port[/protocol]
 *          port[/protocol]
 *          0/icmp[/count]
 *          /path
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sockets are left in bound state for later listen 
 *      in Ns_SockListenXXX.  
 *
 *----------------------------------------------------------------------
 */

static void
PreBind(char *line)
{
#ifdef _WIN32
    return;
#else
    Tcl_HashEntry *hPtr;
    int new, sock, port;
    struct sockaddr_in sa;
    char *next, *str, *addr, *proto;

    for (;line != NULL; line = next) {
        next = strchr(line, ',');
        if (next) {
            *next++ = '\0';
        }
        proto = "tcp";
        addr = "0.0.0.0";
        /* Parse port */
        str = strchr(line, ':');
        if (str) {
            *str++ = '\0';
            port = atoi(str);
            addr = line;
            line = str;
        } else {
            port = atoi(line);
        }
        /* Parse protocol */
        if (*line != '/' && (str = strchr(line,'/'))) {
            *str++ = '\0';
            proto = str;
        }
        
        if (!strcmp(proto,"tcp") && port > 0) {
            if (Ns_GetSockAddr(&sa, addr, port) != NS_OK) {
                Ns_Log(Error, "prebind: tcp: invalid address: %s:%d",
                       addr, port);
                continue;
            }
            hPtr = Tcl_CreateHashEntry(&preboundTcp, (char *) &sa, &new);
            if (!new) {
                Ns_Log(Error, "prebind: tcp: duplicate entry: %s:%d",
                       addr, port);
                continue;
            }
            sock = Ns_SockBind(&sa);
            if (sock == -1) {
                Ns_Log(Error, "prebind: tcp: %s:%d: %s", addr, port,
                       strerror(errno));
                Tcl_DeleteHashEntry(hPtr);
                continue;
            }
            Tcl_SetHashValue(hPtr, sock);
            Ns_Log(Notice, "prebind: tcp: %s:%d = %d", addr, port, sock);
        }

        if (!strcmp(proto,"udp") && port > 0) {
            if (Ns_GetSockAddr(&sa, addr, port) != NS_OK) {
                Ns_Log(Error, "prebind: udp: invalid address: %s:%d",
                       addr, port);
                continue;
            }
            hPtr = Tcl_CreateHashEntry(&preboundUdp, (char *) &sa, &new);
            if (!new) {
                Ns_Log(Error, "prebind: udp: duplicate entry: %s:%d",
                       addr, port);
                continue;
            }
            sock = Ns_SockBindUdp(&sa);
            if (sock == -1) {
                Ns_Log(Error, "prebind: udp: %s:%d: %s", addr, port,
                       strerror(errno));
                Tcl_DeleteHashEntry(hPtr);
                continue;
            }
            Tcl_SetHashValue(hPtr, sock);
            Ns_Log(Notice, "prebind: udp: %s:%d = %d", addr, port, sock);
        }

        if (!strncmp(proto,"icmp",4)) {
            int count = 1;
            /* Parse count */
            str = strchr(str,'/');
            if (str) {
                *(str++) = '\0';
                count = atoi(str);
            }
            while(count--) {
                sock = Ns_SockBindRaw(IPPROTO_ICMP);
                if (sock == -1) {
                    Ns_Log(Error, "prebind: icmp: %s",strerror(errno));
                    continue;
                }
                hPtr = Tcl_CreateHashEntry(&preboundRaw, (char *) sock, &new);
                if (!new) {
                    Ns_Log(Error, "prebind: icmp: duplicate entry");
                    close(sock);
                    continue;
                }
                Tcl_SetHashValue(hPtr, IPPROTO_ICMP);
                Ns_Log(Notice, "prebind: icmp: %d", sock);
            }
        }

        if (Ns_PathIsAbsolute(line)) {
            hPtr = Tcl_CreateHashEntry(&preboundUnix, (char *) line, &new);
            if (!new) {
                Ns_Log(Error, "prebind: unix: duplicate entry: %s",line);
                continue;
            }
            sock = Ns_SockBindUnix(line);
            if (sock == -1) {
                Ns_Log(Error, "prebind: unix: %s: %s", proto, strerror(errno));
                Tcl_DeleteHashEntry(hPtr);
                continue;
            }
            Tcl_SetHashValue(hPtr, sock);
            Ns_Log(Notice, "prebind: unix: %s = %d", line, sock);
        }
    }
#endif
}
