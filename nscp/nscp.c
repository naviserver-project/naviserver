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
    const char *addr;
    unsigned short port;
    bool echo;
    bool commandLogging;
    bool allowLoopbackEmptyUser;
} Mod;

static Ns_ThreadProc EvalThread;
static Mod          *modPtr = NULL;

/*
 * The following structure is allocated for each session.
 */

typedef struct Sess {
    Mod *modPtr;
    const char *user;
    int id;
    bool viaLoopback;
    NS_SOCKET sock;
    struct NS_SOCKADDR_STORAGE sa;
} Sess;

/*
 * The following functions are defined locally.
 */

static Ns_SockProc AcceptProc;
static TCL_OBJCMDPROC_T ExitObjCmd;
static bool Login(const Sess *sessPtr, Tcl_DString *unameDSPtr);
static bool GetLine(NS_SOCKET sock, const char *prompt, Tcl_DString *dsPtr, bool echo)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static void LoadUsers(Mod *localModPtr, const char *server, const char *module)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static Ns_ArgProc ArgProc;
static Ns_TclTraceProc NscpAddCmds;
static TCL_OBJCMDPROC_T NsTclNscpObjCmd;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;
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
    //NS_NONNULL_ASSERT(server != NULL);
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
            Ns_Log(Warning, "nscp[%s]: user entry '%s' contains no colon; ignored.", server,  user);
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
    unsigned short port;
    Ns_ReturnCode  result;
    NS_SOCKET      lsock;

    NS_NONNULL_ASSERT(module != NULL);

    /*
     * Create the listening socket and callback.
     */
    section = Ns_ConfigSectionPath(NULL, server, module, NS_SENTINEL);
    addr = Ns_ConfigString(section, "address", NS_IP_LOOPBACK);
    port = (unsigned short)Ns_ConfigInt(section, "port", 2080);

    lsock = Ns_SockListen(addr, port);
    if (lsock == NS_INVALID_SOCKET) {
        Ns_Log(Error, "nscp[%s]: could not listen on [%s]:%hu", server, addr, port);
        result = NS_ERROR;

    } else {

        Ns_Log(Notice, "nscp[%s]: listening on [%s]:%hu", server, addr, port);

        /*
         * Create a new Mod structure for this instance.
         */
        modPtr = ns_malloc(sizeof(Mod));
        modPtr->server = server;
        modPtr->servPtr = Ns_GetServer(server);
        modPtr->addr = ns_strcopy(addr);
        modPtr->port = port;
        modPtr->echo = Ns_ConfigBool(section, "echopasswd", NS_FALSE);
        modPtr->commandLogging = Ns_ConfigBool(section, "cpcmdlogging", NS_FALSE);
        modPtr->allowLoopbackEmptyUser = Ns_ConfigBool(section, "allowLoopbackEmptyUser", NS_FALSE);

        LoadUsers(modPtr, server, module);

        result = Ns_SockCallback(lsock, AcceptProc, modPtr,
                                 ((unsigned int)NS_SOCK_READ | (unsigned int)NS_SOCK_EXIT));

#ifndef PCLINT_BUG
        if (result == NS_OK) {
            Ns_RegisterProcInfo((ns_funcptr_t)AcceptProc, "nscp", ArgProc);
        }
#else
        if (result == NS_OK) Ns_RegisterProcInfo((ns_funcptr_t)AcceptProc, "nscp", ArgProc);
#endif

        if (server != NULL) {
            if (Ns_TclRegisterTrace(server, NscpAddCmds, server, NS_TCL_TRACE_CREATE) != NS_OK) {
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
ArgProc(Tcl_DString *dsPtr, const void *UNUSED(arg))
{
    /*const Mod *modPtr = arg;*/
    assert(modPtr != NULL);

    Tcl_DStringStartSublist(dsPtr);
    Ns_DStringPrintf(dsPtr, "%s %d", modPtr->addr, modPtr->port);
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
AcceptProc(NS_SOCKET sock, void *UNUSED(arg), unsigned int why)
{
    bool success = NS_TRUE;

    if (why == (unsigned int)NS_SOCK_EXIT) {
        Ns_Log(Notice, "nscp: shutdown");
        (void )ns_sockclose(sock);
        success = NS_FALSE;

    } else {
        /*Mod       *modPtr = arg;*/
        Sess      *sessPtr;
        socklen_t  len;

        sessPtr = ns_malloc(sizeof(Sess));
        sessPtr->modPtr = modPtr;
        len = (socklen_t)sizeof(struct sockaddr_in);
        sessPtr->sock = Ns_SockAccept(sock, (struct sockaddr *) &sessPtr->sa, &len);
        if (sessPtr->sock == NS_INVALID_SOCKET) {
            Ns_Log(Error, "nscp: accept() failed: %s",
                   ns_sockstrerror(ns_sockerrno));
            ns_free(sessPtr);
            success = NS_FALSE;

        } else {
            static int next = 0;

            sessPtr->id = ++next;
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

    ncmd = 0;
    while (1 /* was "stop == 0", but stop is not modified in the loop */) {
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

        if (n > 1 && buf[n-1] == '\n' && buf[n-2] == '\r') {
            buf[n-2] = '\n';
            --n;
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
            if ( UCHAR(buf[1]) == TN_EOF) {
                result = NS_FALSE;
                goto bail;
            } else if (UCHAR(buf[1]) == TN_IP) {
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

        Tcl_DStringAppend(dsPtr, buf, (TCL_SIZE_T)n);
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

        if (*user == '\0' && sessPtr->viaLoopback) {
            ok = NS_TRUE;
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
            status = Ns_AuthorizeUser((Ns_Server*)(sessPtr->modPtr->servPtr), user, pass, &authority);
            /*fprintf(stderr, "NSCP LOGIN server '%s' -> %s %d (%s)\n", sessPtr->modPtr->server,
              authority, status, Ns_ReturnCodeString(status));*/
            Ns_Log(Notice, "nscp login user '%s' -> %s", user,  Ns_ReturnCodeString(status));
            if (status == NS_OK) {
                ok = NS_TRUE;
                nscpUserLookup = NS_FALSE;
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

            if (hPtr != NULL) {
                const char *encpass = Tcl_GetHashValue(hPtr);
                char  buf[NS_ENCRYPT_BUFSIZE];

                (void) Ns_Encrypt(pass, encpass, buf);

                if (STREQ(buf, encpass)) {
                    ok = NS_TRUE;
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
        Ns_Log(Warning, "nscp: login failed: '%s'", (user != NULL) ? user : "?");
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
NscpAddCmds(Tcl_Interp *interp, const void *UNUSED(arg))
{
    /*const char *server = arg;*/

    (void)TCL_CREATEOBJCOMMAND(interp, "nscp", NsTclNscpObjCmd, NULL, NULL);

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
NscpUsersObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
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
