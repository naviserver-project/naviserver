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
 * nscp.c --
 *
 *      Simple control port module for NaviServer which allows
 *      one to telnet to a specified port, login, and issue
 *      Tcl commands.
 */

#include "ns.h"

/*
 * The following structure is allocated each instance of
 * the loaded module.
 */

typedef struct Mod {
    Tcl_HashTable users;
    const char *server;
    const Ns_Server *servPtr;
    unsigned short port;
    bool echo;
    bool commandLogging;
    bool allowLoopbackEmptyUser;
} Mod;

static Ns_ThreadProc EvalThread;
#define NSCP_MAX_LISTEN_ADDRESSES 64

typedef struct Listener {
    Mod *modPtr;
    char *addr;
    unsigned short port;
} Listener;

/*
 * The following structure is allocated for each session.
 */

typedef struct Sess {
    Mod *modPtr;
    const char *user;
    int id;
    bool viaLoopback;
    NS_SOCKET sock;
    socklen_t peerLength, peerCapacity;
    struct NS_SOCKADDR_STORAGE sa;
} Sess;

/*
 * The following functions are defined locally.
 */

static Ns_SockProc AcceptProc;
static TCL_OBJCMDPROC_T ExitObjCmd;
static bool Login(const Sess *sessPtr, Tcl_DString *unameDSPtr);
static bool GetLine(NS_SOCKET sock, const char *prompt, Tcl_DString *dsPtr, bool echo)
    NS_GNUC_NONNULL(2,3);
static void LoadUsers(Mod *localModPtr, const char *server, const char *module)
    NS_GNUC_NONNULL(1,3);
static bool ResolveHost(Tcl_DString *dsPtr, const char *host)
    NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode ResolveAddresses(const char *value, Tcl_DString *addressesPtr)
    NS_GNUC_NONNULL(1,2);
static Ns_ArgProc ArgProc;
static Ns_TclTraceProc NscpAddCmds;
static TCL_OBJCMDPROC_T NsTclNscpObjCmd;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;
NS_EXPORT Ns_ModuleInfoProc Ns_ModuleGetInfo;

/*
 * Provide module build and ABI information for runtime introspection.
 */
NS_EXPORT void
Ns_ModuleGetInfo(Ns_ModuleInfo *infoPtr)
{
    Ns_ModuleInfoInit(infoPtr, NS_MODULE_INFO_VERSION,
                      "nscp",
                      PACKAGE_VERSION,
                      PACKAGE_TAG,
                      "module",
                      1u);
}

/*
 * The following values are sent to the telnet client to enable
 * and disable password prompt echo.
 */

#define TN_IAC  255u
#define TN_WILL 251u
#define TN_WONT 252u
#define TN_DO   253u
#define TN_DONT 254u
#define TN_EOF  236u
#define TN_IP   244u
#define TN_ECHO   1u

static const unsigned char do_echo[]    = {TN_IAC, TN_DO,   TN_ECHO};
static const unsigned char dont_echo[]  = {TN_IAC, TN_DONT, TN_ECHO};
static const unsigned char will_echo[]  = {TN_IAC, TN_WILL, TN_ECHO};
static const unsigned char wont_echo[]  = {TN_IAC, TN_WONT, TN_ECHO};

/*
 * Define the version of the module (usually 1).
 */

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;


/*
 *----------------------------------------------------------------------
 *
 * LoadUsers --
 *
 *     Initialize the hash table of authorized users.  Fallback auth user
 *     authentication.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initialize and populate user hash table. The entries are compatible
 *      with /etc/passwd (i.e., username followed by password separated by
 *      colons).
 *
 *----------------------------------------------------------------------
 */

static void
LoadUsers(Mod *localModPtr, const char *server, const char *module)
{
    Ns_Set     *set = NULL;
    size_t      i;

    NS_NONNULL_ASSERT(localModPtr != NULL);
    NS_NONNULL_ASSERT(module != NULL);

    Tcl_InitHashTable(&localModPtr->users, TCL_STRING_KEYS);
    (void) Ns_ConfigSectionPath(&set, server, module, "users", NS_SENTINEL);

    /*
     * Process the setup ns_set
     */
    for (i = 0u; set != NULL && i < Ns_SetSize(set); ++i) {
        const char    *key  = Ns_SetKey(set, i);
        const char    *user = Ns_SetValue(set, i);
        const char    *passPart;
        char          *scratch, *p, *end;
        size_t         userLength;
        Tcl_HashEntry *hPtr;
        int            isNew;

        if (!STRIEQ(key, "user")) {
            continue;
        }
        passPart = strchr(user, INTCHAR(':'));
        if (passPart == NULL) {
            Ns_Log(Warning, "nscp[%s]: user entry '%s' contains no colon; ignored.",
                   server == NULL ? "global" : server,  user);
            continue;
        }

        /*
         * Copy string to avoid conflicts with const property.
         */
        p = scratch = ns_strdup(user);

        /*
         * Terminate user part.
         */
        userLength = (size_t)(passPart - user);
        *(p + userLength) = '\0';

        hPtr = Tcl_CreateHashEntry(&localModPtr->users, p, &isNew);
        if (isNew != 0) {
            Ns_Log(Notice, "nscp[%s]: added user: \"%s\"", server, p);
        } else {
            Ns_Log(Warning, "nscp[%s]: duplicate user: \"%s\"", server, p);
            ns_free(Tcl_GetHashValue(hPtr));
        }
        /*
         * Advance to password part in scratch copy.
         */
        p += userLength + 1u;

        /*
         * look for end of password.
         */
        end = strchr(p, INTCHAR(':'));
        if (end != NULL) {
            *end = '\0';
        }

        /*
         * Save the password.
         */
        Tcl_SetHashValue(hPtr, ns_strdup(p));

        ns_free(scratch);
    }
    if (localModPtr->users.numEntries == 0) {
        Ns_Log(Warning, "nscp[%s]: no authorized users", server);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ResolveHost --
 *
 *      Resolve a listening hostname to numeric IP addresses. When
 *      getaddrinfo() is available, omit outbound AI_ADDRCONFIG filtering:
 *      containers can support ::1 without a non-loopback IPv6 interface.
 *
 * Results:
 *      NS_TRUE if at least one supported address was found, NS_FALSE
 *      otherwise.
 *
 * Side effects:
 *      Appends addresses as Tcl list elements to dsPtr. May perform name
 *      service lookups and log resolution errors.
 *
 *----------------------------------------------------------------------
 */
static bool
ResolveHost(Tcl_DString *dsPtr, const char *host)
{
#ifdef HAVE_GETADDRINFO
    struct addrinfo hints, *addresses = NULL;
    const struct addrinfo *entry;
    int status;
    bool found = NS_FALSE;

    memset(&hints, 0, sizeof(hints));
#ifdef HAVE_IPV6
    hints.ai_family = AF_UNSPEC;
#else
    hints.ai_family = AF_INET;
#endif
    hints.ai_socktype = SOCK_STREAM;
    status = getaddrinfo(host, NULL, &hints, &addresses);
    if (status != 0) {
        Ns_Log(Error, "nscp: could not resolve listening address '%s': %s", host, gai_strerror(status));
    } else {
        for (entry = addresses; entry != NULL; entry = entry->ai_next) {
            char ip[NS_IPADDR_SIZE];

            if (entry->ai_family != AF_INET
#ifdef HAVE_IPV6
                && entry->ai_family != AF_INET6
#endif
                ) {continue;}
            if (ns_inet_ntop(entry->ai_addr, ip, sizeof(ip)) != NULL) {
                Tcl_DStringAppendElement(dsPtr, ip);
                found = NS_TRUE;
            }
        }
        freeaddrinfo(addresses);
    }
    return found;
#else
    return Ns_GetAllAddrByHost(dsPtr, host);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * ResolveAddresses --
 *
 *      Resolve a nonempty Tcl list of listening addresses and hostnames
 *      before opening any listener. Normalize numeric addresses and remove
 *      duplicates, enforcing NSCP_MAX_LISTEN_ADDRESSES.
 *
 * Results:
 *      NS_OK on success, NS_ERROR for an invalid list, failed resolution,
 *      invalid resolved address, or too many distinct addresses.
 *
 * Side effects:
 *      Appends unique numeric addresses as Tcl list elements to
 *      addressesPtr, which may contain partial results on error. May
 *      perform name service lookups and log configuration errors.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ResolveAddresses(const char *value, Tcl_DString *addressesPtr)
{
    const char   **hosts = NULL;
    TCL_SIZE_T     count, i;
    Tcl_HashTable  seen;
    Ns_ReturnCode  result = NS_OK;

    if (Tcl_SplitList(NULL, value, &count, &hosts) != TCL_OK || count < 1) {
        Ns_Log(Error, "nscp: address must be a nonempty Tcl list: '%s'", value);
        if (hosts != NULL) {Tcl_Free((char *)hosts);}
        return NS_ERROR;
    }
    Tcl_InitHashTable(&seen, TCL_STRING_KEYS);
    for (i = 0; i < count && result == NS_OK; i++) {
        struct NS_SOCKADDR_STORAGE sa;
        Tcl_DString resolved;
        const char **ips = NULL;
        TCL_SIZE_T   nips, j;

        Tcl_DStringInit(&resolved);
        memset(&sa, 0, sizeof(sa));
        if (ns_inet_pton((struct sockaddr *)&sa, hosts[i]) > 0) {
            Tcl_DStringAppendElement(&resolved, hosts[i]);
        } else if (!ResolveHost(&resolved, hosts[i])) {
            Ns_Log(Error, "nscp: could not resolve listening address '%s'", hosts[i]);
            result = NS_ERROR;
        }
        if (result == NS_OK) {
            if (Tcl_SplitList(NULL, Tcl_DStringValue(&resolved), &nips, &ips) != TCL_OK || nips < 1) {
                result = NS_ERROR;
            } else {
                for (j = 0; j < nips; j++) {
                    char ip[NS_IPADDR_SIZE];
                    int  isNew;

                    memset(&sa, 0, sizeof(sa));
                    if (ns_inet_pton((struct sockaddr *)&sa, ips[j]) <= 0
                        || ns_inet_ntop((const struct sockaddr *)&sa, ip, sizeof(ip)) == NULL) {
                        Ns_Log(Error, "nscp: invalid resolved address '%s'", ips[j]);
                        result = NS_ERROR;
                        break;
                    }
                    (void)Tcl_CreateHashEntry(&seen, ip, &isNew);
                    if (isNew != 0) {
                        if (seen.numEntries > NSCP_MAX_LISTEN_ADDRESSES) {
                            Ns_Log(Error, "nscp: at most %d distinct listening addresses are supported",
                                   NSCP_MAX_LISTEN_ADDRESSES);
                            result = NS_ERROR;
                            break;
                        }
                        Tcl_DStringAppendElement(addressesPtr, ip);
                    }
                }
            }
        }
        if (ips != NULL) {Tcl_Free((char *)ips);}
        Tcl_DStringFree(&resolved);
    }
    Tcl_DeleteHashTable(&seen);
    Tcl_Free((char *)hosts);
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Load the config parameters, setup the structures, and
 *      listen on the control port.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Server will listen for control connections on specified
 *      address and port.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    const char    *addr, *section;
    const char   **addresses = NULL;
    unsigned short port;
    Ns_ReturnCode  result = NS_ERROR;
    Tcl_DString    resolved;
    TCL_SIZE_T     count = 0, i;
    size_t         listening = 0u;
    Mod           *modPtr;

    NS_NONNULL_ASSERT(module != NULL);

    section = Ns_ConfigSectionPath(NULL, server, module, NS_SENTINEL);
    addr = Ns_ConfigString(section, "address", NS_IP_LOOPBACK);
    port = (unsigned short)Ns_ConfigInt(section, "port", 2080);
    Tcl_DStringInit(&resolved);
    if (ResolveAddresses(addr, &resolved) != NS_OK
        || Tcl_SplitList(NULL, Tcl_DStringValue(&resolved), &count, &addresses) != TCL_OK) {
        Tcl_DStringFree(&resolved);
        return NS_ERROR;
    }

    /* All listeners share this instance's users and authentication policy. */
    modPtr = ns_malloc(sizeof(Mod));
    modPtr->server = server;
    modPtr->servPtr = Ns_GetServer(server);
    modPtr->port = port;
    modPtr->echo = Ns_ConfigBool(section, "echopasswd", NS_FALSE);
    modPtr->commandLogging = Ns_ConfigBool(section, "cpcmdlogging", NS_FALSE);
    modPtr->allowLoopbackEmptyUser = Ns_ConfigBool(section, "allowLoopbackEmptyUser", NS_FALSE);
    LoadUsers(modPtr, server, module);

    for (i = 0; i < count; i++) {
        NS_SOCKET lsock = Ns_SockListen(addresses[i], modPtr->port);
        Listener *listener;

        if (lsock == NS_INVALID_SOCKET) {
            Ns_Log(Warning, "nscp[%s]: could not listen on [%s]:%hu; trying remaining addresses",
                   server, addresses[i], modPtr->port);
            continue;
        }
        if (modPtr->port == 0u) {
            struct NS_SOCKADDR_STORAGE sa;
            socklen_t len = (socklen_t)sizeof(sa);

            /* Port zero selects one ephemeral port shared by all addresses. */
            if (getsockname(lsock, (struct sockaddr *)&sa, &len) != 0) {
                (void)ns_sockclose(lsock);
                continue;
            }
            modPtr->port = Ns_SockaddrGetPort((const struct sockaddr *)&sa);
        }
        listener = ns_malloc(sizeof(Listener));
        listener->modPtr = modPtr;
        listener->addr = ns_strdup(addresses[i]);
        listener->port = modPtr->port;
        if (Ns_SockCallback(lsock, AcceptProc, listener,
                            ((unsigned int)NS_SOCK_READ | (unsigned int)NS_SOCK_EXIT)) != NS_OK) {
            (void)ns_sockclose(lsock);
            ns_free(listener->addr);
            ns_free(listener);
            continue;
        }
        listening++;
        Ns_Log(Notice, "nscp[%s]: listening on [%s]:%hu", server, addresses[i], modPtr->port);
    }
    Tcl_Free((char *)addresses);
    Tcl_DStringFree(&resolved);

    if (listening == 0u) {
        Tcl_HashSearch search;
        Tcl_HashEntry *entry;

        Ns_Log(Error, "nscp[%s]: no listening address is available", server);
        for (entry = Tcl_FirstHashEntry(&modPtr->users, &search); entry != NULL;
             entry = Tcl_NextHashEntry(&search)) {
            ns_free(Tcl_GetHashValue(entry));
        }
        Tcl_DeleteHashTable(&modPtr->users);
        ns_free(modPtr);
    } else {
        Ns_RegisterProcInfo((ns_funcptr_t)AcceptProc, "nscp", ArgProc);
        result = NS_OK;
        if (server != NULL) {
            if (Ns_TclRegisterTrace(server, NscpAddCmds, modPtr, NS_TCL_TRACE_CREATE) != NS_OK) {
                result = NS_ERROR;
            } else {
                Ns_RegisterProcInfo((ns_funcptr_t)NscpAddCmds, "nscp:initinterp", NULL);
            }
        } else {
            Ns_Log(Notice, "nscp: the command 'nscp' cannot be registered when the module is loaded globally");
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ArgProc --
 *
 *      Append listen port info for query callback.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
ArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const Listener *listener = arg;

    assert(listener != NULL);
    Tcl_DStringStartSublist(dsPtr);
    Tcl_DStringAppendElement(dsPtr, listener->addr);
    Ns_DStringPrintf(dsPtr, " %hu", listener->port);
    Tcl_DStringEndSublist(dsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * AcceptProc --
 *
 *      Socket callback to accept a new connection.
 *
 * Results:
 *      NS_TRUE to keep listening unless shutdown is in progress.
 *
 * Side effects:
 *      New EvalThread will be created.
 *
 *----------------------------------------------------------------------
 */

static bool
AcceptProc(NS_SOCKET sock, void *arg, unsigned int why)
{
    Listener *listener = arg;
    Mod      *modPtr = listener->modPtr;
    bool      success = NS_TRUE;

    if (why == (unsigned int)NS_SOCK_EXIT) {
        Ns_Log(Notice, "nscp: shutdown");
        (void )ns_sockclose(sock);
        /* Keep startup metadata valid for concurrent callback introspection
         * during shutdown, just like the shared module/user configuration. */
        success = NS_FALSE;

    } else {
        Sess      *sessPtr;
        socklen_t  len;

        sessPtr = ns_malloc(sizeof(Sess));
        sessPtr->modPtr = modPtr;
        /* accept() needs the full storage size, including IPv6 addresses. */
        len = (socklen_t)sizeof(sessPtr->sa);
        sessPtr->peerCapacity = len;
        sessPtr->sock = Ns_SockAccept(sock, (struct sockaddr *) &sessPtr->sa, &len);
        sessPtr->peerLength = len;
        if (sessPtr->sock == NS_INVALID_SOCKET) {
            Ns_Log(Error, "nscp: accept() failed: %s",
                   ns_sockstrerror(ns_sockerrno));
            ns_free(sessPtr);
            success = NS_FALSE;

        } else {
            static int next = 0;

            Ns_MasterLock();
            sessPtr->id = ++next;
            Ns_MasterUnlock();
            Ns_ThreadCreate(EvalThread, sessPtr, 0, NULL);
        }
    }
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * EvalThread --
 *
 *      Thread to read and evaluate commands from remote.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on commands.
 *
 *----------------------------------------------------------------------
 */

static void
EvalThread(void *arg)
{
    Tcl_Interp *interp = NULL;
    Tcl_DString ds, unameDS;
    char        ipString[NS_IPADDR_SIZE];
    int         ncmd, stop;
    Sess       *sessPtr = arg;
    const char *server = sessPtr->modPtr->server;

    /*
     * Initialize the thread and login the user.
     */
    ns_inet_ntop((struct sockaddr *)&(sessPtr->sa), ipString, sizeof(ipString));

    Tcl_DStringInit(&ds);
    Tcl_DStringInit(&unameDS);
    Ns_DStringPrintf(&ds, "-nscp:%d-", sessPtr->id);
    Ns_ThreadSetName("%s", ds.string);
    Tcl_DStringSetLength(&ds, 0);
    sessPtr->viaLoopback = STREQ(ipString, "::1") || (strncmp(ipString, "127.", 4u) == 0);

    Ns_Log(Notice, "nscp: %s connected (loopback %d)", ipString, sessPtr->viaLoopback);
    if (!Login(sessPtr, &unameDS)) {
        goto done;
    }

    sessPtr->user = Tcl_DStringValue(&unameDS);

    /*
     * Loop until the remote shuts down, evaluating complete
     * commands.
     */

    interp = Ns_TclAllocateInterp(server);

    /*
     * Create a special exit command for this interp only.
     */

    stop = 0;
    (void)TCL_CREATEOBJCOMMAND(interp, "exit", ExitObjCmd, (ClientData) &stop, NULL);

    /*
     * The nscp command loop runs until the variable 'stop' is set non-zero.
     * The loop itself never modifies this variable -- it is changed only by
     * the "exit" command, which updates 'stop' through the pointer passed
     * when the command was registered. This mechanism provides a simple,
     * thread-local way to signal graceful termination of the loop.
     */
    ncmd = 0;
    while (stop == 0) {
        TCL_SIZE_T  len;
        const char *resultString;
        char        buf[64];

        Tcl_DStringSetLength(&ds, 0);
        ++ncmd;
retry:
        snprintf(buf, sizeof(buf), "%s:nscp %d> ", server, ncmd);
        for (;;) {
            if (!GetLine(sessPtr->sock, buf, &ds, NS_TRUE)) {
                goto done;
            }
            if (Tcl_CommandComplete(ds.string) != 0) {
                break;
            }
            snprintf(buf, sizeof(buf), "%s:nscp %d>>> ", server, ncmd);
        }
        while (ds.length > 0 && ds.string[ds.length-1] == '\n') {
            Tcl_DStringSetLength(&ds, ds.length-1);
        }
        if (ds.length == 0) {
            goto retry; /* Empty command - try again. */
        }

        if (sessPtr->modPtr->commandLogging) {
            Ns_Log(Notice, "nscp: %s %d: %s", sessPtr->user, ncmd, ds.string);
        }

        if (Tcl_RecordAndEval(interp, ds.string, 0) != TCL_OK) {
            (void) Ns_TclLogErrorInfo(interp, "\n(context: nscp)");
        }
        Tcl_AppendResult(interp, "\r\n", NS_SENTINEL);
        resultString = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &len);
        while (len > 0) {
            ssize_t sent = ns_send(sessPtr->sock, resultString, (size_t)len, 0);
            if (sent <= 0) {
                goto done;
            }
            len -= (TCL_SIZE_T)sent;
            resultString += sent;
        }

        if (sessPtr->modPtr->commandLogging) {
            Ns_Log(Notice, "nscp: %s %d: done", sessPtr->user, ncmd);
        }
    }
done:
    Tcl_DStringFree(&ds);
    Tcl_DStringFree(&unameDS);
    if (interp != NULL) {
        Ns_TclDeAllocateInterp(interp);
    }
    Ns_Log(Notice, "nscp: %s disconnected",
           ns_inet_ntop((struct sockaddr *)&(sessPtr->sa), ipString, sizeof(ipString)));
    ns_sockclose(sessPtr->sock);
    ns_free(sessPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * GetLine --
 *
 *      Prompt for a line of input from the remote.  \r\n sequences
 *      are translated to \n.
 *
 * Results:
 *      NS_TRUE if line received, NS_FALSE if remote dropped.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
GetLine(NS_SOCKET sock, const char *prompt, Tcl_DString *dsPtr, bool echo)
{
    char    buf[2048];
    int     retry = 0;
    bool    result = NS_FALSE;
    ssize_t n;
    size_t  promptLength;

    NS_NONNULL_ASSERT(prompt != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    /*
     * Suppress output on things like password prompts.
     */

    if (!echo) {
        (void)ns_send(sock, (const void*)will_echo, 3u, 0);
        (void)ns_send(sock, (const void*)dont_echo, 3u, 0);
        (void)ns_recv(sock, buf, sizeof(buf), 0); /* flush client ack thingies */
    }
    promptLength = strlen(prompt);
    if (ns_send(sock, prompt, promptLength, 0) != (ssize_t)promptLength) {
        goto bail;
    }

    do {
        n = ns_recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            result = NS_FALSE;
            goto bail;
        }

        /*
         * This EOT checker cannot happen in the context of telnet.
         */
        if (n == 1 && buf[0] == '\4') {
            result = NS_FALSE;
            goto bail;
        }

        /*
         * Deal with telnet IAC commands in some sane way.
         */

        if (n > 1 && UCHAR(buf[0]) == TN_IAC) {
            if ((UCHAR(buf[1]) == TN_EOF) || (UCHAR(buf[1]) == TN_IP)) {
                result = NS_FALSE;
                goto bail;
            } else if ((UCHAR(buf[1]) == TN_WONT) && (retry < 2)) {
                /*
                 * It seems like the flush at the bottom of this func
                 * does not always get all the acks, thus an echo ack
                 * showing up here. Not clear why this would be.  Need
                 * to investigate further. For now, breeze past these
                 * (within limits).
                 */
                retry++;
                continue;
            } else {
                Ns_Log(Warning, "nscp: "
                       "unsupported telnet IAC code received from client");
                result = NS_FALSE;
                goto bail;
            }
        }

        {
            TCL_SIZE_T readPos = dsPtr->length, writePos;

            /*
             * Normalize every CRLF, not just the last one in a socket read.
             * A paste can contain several lines, and a CRLF can straddle
             * reads. Tcl requires backslash immediately followed by newline
             * for a continuation. Preserve bare carriage returns.
             */
            if (readPos > 0 && dsPtr->string[readPos - 1] == '\r') {
                --readPos;
            }
            Tcl_DStringAppend(dsPtr, buf, (TCL_SIZE_T)n);
            writePos = readPos;
            while (readPos < dsPtr->length) {
                if (dsPtr->string[readPos] == '\r'
                    && readPos + 1 < dsPtr->length
                    && dsPtr->string[readPos + 1] == '\n') {
                    ++readPos;
                }
                dsPtr->string[writePos++] = dsPtr->string[readPos++];
            }
            Tcl_DStringSetLength(dsPtr, writePos);
        }
        result = NS_TRUE;

    } while (buf[n-1] != '\n');

 bail:
    if (!echo) {
        (void)ns_send(sock, (const void*)wont_echo, 3u, 0);
        (void)ns_send(sock, (const void*)do_echo, 3u, 0);
        (void)ns_recv(sock, buf, sizeof(buf), 0); /* flush client ack thingies */
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Login --
 *
 *      Attempt to login the user.
 *
 * Results:
 *      NS_TRUE if login ok, NS_FALSE otherwise.
 *
 * Side effects:
 *      Stores user's login name into unameDSPtr.
 *
 *----------------------------------------------------------------------
 */

static bool
Login(const Sess *sessPtr, Tcl_DString *unameDSPtr)
{
    Tcl_DString uds, pds, msgDs;
    const char *user = NULL;
    const char *authPath = "credentials-not-read", *authAuthority = "none";
    Ns_ReturnCode authStatus = NS_ERROR;
    bool        ok = NS_FALSE;

    Tcl_DStringInit(&uds);
    Tcl_DStringInit(&pds);
    if (GetLine(sessPtr->sock, "login: ", &uds, NS_TRUE)
        && GetLine(sessPtr->sock, "Password: ", &pds, sessPtr->modPtr->echo)
        ) {
        const char *pass;
        bool        nscpUserLookup = NS_FALSE;

        user = Ns_StrTrim(uds.string);
        pass = Ns_StrTrim(pds.string);

        /*
         * Authentication logic:
         *   - If the username is empty, the connection originates from a
         *     loopback address, and the configuration variable permitting
         *     unauthenticated access is enabled, accept the login without
         *     further authentication.
         *   - Otherwise, if a username is provided, attempt to authenticate
         *     using the nsperm module (if loaded). If nsperm is not
         *     available, fall back to using the control port users.
         */

        if (*user == '\0' && sessPtr->viaLoopback && sessPtr->modPtr->allowLoopbackEmptyUser) {
            ok = NS_TRUE;
            authPath = "loopback-empty-user";
            authStatus = NS_OK;
        } else if (sessPtr->modPtr->server == NULL) {
            Ns_Log(Warning, "nscp: to use AuthorizeUser, register the nscp module for a server, not globally");
            nscpUserLookup = NS_TRUE;

        } else {
            Ns_ReturnCode status;
            const char *authority = NULL;

            /*fprintf(stderr, "NSCP LOGIN server '%s'\n", sessPtr->modPtr->server);*/

            /*
             * We have the following logic:
             *   when ok        -> authorization was successful
             *   nscpUserLookup -> try user lookup of nscp module
             *
             * Ns_AuthorizeUser() return codes:
             *   NS_OK -> ok = NS_TRUE, nscpUserLookup = NS_FALSE
             *   NS_FORBIDDEN -> ok = NS_FALSE, nscpUserLookup = NS_FALSE
             *   NS_UNAUTHORIZED -> ok = NS_FALSE, nscpUserLookup = NS_TRUE
             *
             */
            status = Ns_AuthorizeUser((const Ns_Server*)(sessPtr->modPtr->servPtr), user, pass, &authority);
            authPath = "user-authorizer";
            authStatus = status;
            authAuthority = authority != NULL ? authority : "none";
            /*fprintf(stderr, "NSCP LOGIN server '%s' -> %s %d (%s)\n", sessPtr->modPtr->server,
              authority, status, Ns_ReturnCodeString(status));*/

            Ns_Log(Notice, "nscp login user '%s' -> %s%s%s",
                   user,
                   Ns_ReturnCodeString(status),
                   authority != NULL ? " authority " : "",
                   authority != NULL ? authority : "");

            /*
             * Do not allow a shipped default nsperm administrator credential to
             * unlock the nscp Tcl command shell. Ns_AuthorizeUser() may authenticate
             * through a chain of providers; the authority string identifies the
             * provider that accepted the login. Keep this check local to nscp so
             * nsperm can continue to warn for general use, while the control-port
             * command-execution surface fails closed for the reported default
             * credential.
             */
            if (status == NS_OK) {
                if (authority != NULL
                    && STREQ(authority, "nsperm")
                    && STREQ(user, "nsadmin")
                    && STREQ(pass, "x")) {
                    Ns_Log(Security,
                           "nscp: refusing login for user 'nsadmin' with the shipped "
                           "default nsperm password. The nscp control port provides Tcl "
                           "command execution in the server process. Change the nsadmin "
                           "password before using nscp.");
                    ok = NS_FALSE;
                    authPath = "shipped-default-refused";
                    authStatus = NS_FORBIDDEN;
                    nscpUserLookup = NS_FALSE;
                } else {
                    ok = NS_TRUE;
                    nscpUserLookup = NS_FALSE;
                }

            } else if (status == NS_FORBIDDEN) {
                ok = NS_FALSE;
                nscpUserLookup = NS_FALSE;

            } else {
                ok = NS_FALSE;
                nscpUserLookup = NS_TRUE;
            }
        }
        if (nscpUserLookup) {
            const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&sessPtr->modPtr->users, user);

            authPath = "configured-user-fallback";
            authStatus = NS_UNAUTHORIZED;

            if (hPtr != NULL) {
                const char *encpass = Tcl_GetHashValue(hPtr);
                char  buf[NS_ENCRYPT_BUFSIZE];

                (void) Ns_Encrypt(pass, encpass, buf);

                if (STREQ(buf, encpass)) {
                    ok = NS_TRUE;
                    authStatus = NS_OK;
                }
            } else {
                Ns_Log(Warning, "nscp: no such global user: %s", user);
            }
        }
    }

    /*
     * Report the result of the login to the user.
     */

    Tcl_DStringInit(&msgDs);
    if (ok) {
        Ns_Log(Notice, "nscp: %s logged in", user);
        Tcl_DStringAppend(unameDSPtr, user, TCL_INDEX_NONE);
        Ns_DStringPrintf(&msgDs,
            "\nWelcome to %s running at %s (pid %d)\n"
            "%s/%s for %s built on %s\nTag: %s\n",
            sessPtr->modPtr->server,
            Ns_InfoNameOfExecutable(), Ns_InfoPid(),
            Ns_InfoServerName(), Ns_InfoServerVersion(),
            Ns_InfoPlatform(), Ns_InfoBuildDate(), Ns_InfoTag());
    } else {
        struct NS_SOCKADDR_STORAGE peer = {0};
        socklen_t peerLength = (socklen_t)sizeof(peer);
        char acceptedBuffer[NS_IPADDR_SIZE], actualBuffer[NS_IPADDR_SIZE];
        const char *acceptedPeer, *actualPeer = NULL;

        /* The independent peer lookup helps distinguish address truncation
         * from configuration/provider failures. Never log passwords/hashes. */
        acceptedPeer = ns_inet_ntop((const struct sockaddr *)&sessPtr->sa,
                                    acceptedBuffer, sizeof(acceptedBuffer));
        if (getpeername(sessPtr->sock, (struct sockaddr *)&peer, &peerLength) == 0) {
            actualPeer = ns_inet_ntop((const struct sockaddr *)&peer,
                                     actualBuffer, sizeof(actualBuffer));
        }
        Ns_Log(Warning, "nscp: login failed: '%s' "
               "(accepted-peer %s, family %d, address-bytes %zu, accept-buffer-bytes %zu, storage %zu; "
               "socket-peer %s, family %d; loopback %d, allowLoopbackEmptyUser %d; "
               "auth-path %s, authority %s, status %s)",
               user != NULL ? user : "?",
               acceptedPeer != NULL ? acceptedPeer : "unavailable",
               (int)((const struct sockaddr *)&sessPtr->sa)->sa_family,
               (size_t)sessPtr->peerLength, (size_t)sessPtr->peerCapacity, sizeof(sessPtr->sa),
               actualPeer != NULL ? actualPeer : "unavailable", (int)((const struct sockaddr *)&peer)->sa_family,
               sessPtr->viaLoopback, sessPtr->modPtr->allowLoopbackEmptyUser,
               authPath, authAuthority, Ns_ReturnCodeString(authStatus));
        Tcl_DStringAppend(&msgDs, "Access denied!\n", 15);
    }
    (void) ns_send(sessPtr->sock, msgDs.string, (size_t)msgDs.length, 0);

    Tcl_DStringFree(&msgDs);
    Tcl_DStringFree(&uds);
    Tcl_DStringFree(&pds);

    return ok;
}


/*
 *----------------------------------------------------------------------
 *
 * ExitObjCmd --
 *
 *      Implements "exit", a special exit command for nscp.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ExitObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (unlikely(Ns_ParseObjv(NULL, NULL, interp, 1, objc, objv) != NS_OK)) {
        result = TCL_ERROR;

    } else {
        int *stopPtr = (int *) clientData;

        *stopPtr = 1;
        Ns_TclPrintfResult(interp, "\nGoodbye!");
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsNscpAddCmds --
 *
 *      Add the nscp commands.
 *
 * Results:
 *      TCL_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
NscpAddCmds(Tcl_Interp *interp, const void *arg)
{
    (void)TCL_CREATEOBJCOMMAND(interp, "nscp", NsTclNscpObjCmd, (ClientData)arg, NULL);

    return TCL_OK;
}
/*
 *----------------------------------------------------------------------
 *
 * NscpUsersObjCmd --
 *
 *      Implements "nscp users". Lists all users known to nscp.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
NscpUsersObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Mod *modPtr = clientData;
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_HashSearch  search;
        Tcl_HashEntry  *hPtr = Tcl_FirstHashEntry(&modPtr->users, &search);
        Tcl_Obj        *resultObj = Tcl_NewListObj(0, NULL);

        while (hPtr != NULL) {
            char *userName = Tcl_GetHashKey(&modPtr->users, hPtr);

            Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(userName, TCL_INDEX_NONE));
            hPtr = Tcl_NextHashEntry(&search);
        }

        Tcl_SetObjResult(interp, resultObj);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclNscpObjCmd --
 *
 *      Implements "nscp".
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
NsTclNscpObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"users", NscpUsersObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
