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
 * nsperm --
 *
 *      Permissions
 */

#include "ns.h"

/*
 * The following flags are for user record
 */

#define USER_FILTER_ALLOW     1
#define USER_CLEAR_TEXT       2

/*
 * The following flags are for permission record
 */

#define PERM_IMPLICIT_ALLOW   1

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

static const char *NS_EMPTY_STRING = "";

/*
 * The following structure is allocated for each instance of the module.
 */

typedef struct PServer {
    const char *server;
    Ns_Server *servPtr;
    Tcl_HashTable users;
    Tcl_HashTable groups;
    Ns_RWLock lock;
} PServer;

/*
 * The "users" hash table points to this kind of data:
 */

typedef struct {
    int flags;
    char pwd[NS_ENCRYPT_BUFSIZE];
    Tcl_HashTable groups;
    Tcl_HashTable nets;
    Tcl_HashTable masks;
    Tcl_HashTable hosts;
} User;

/*
 * The "groups" hash table points to this kind of data:
 */

typedef struct {
    Tcl_HashTable users;
} Group;

/*
 * The urlspecific data referenced by uskey hold pointers to these:
 */

typedef struct {
    int flags;
    char *baseurl;
    Tcl_HashTable allowuser;
    Tcl_HashTable denyuser;
    Tcl_HashTable allowgroup;
    Tcl_HashTable denygroup;
} Perm;

/*
 * Local functions defined in this file
 */

static Ns_TclTraceProc AddCmds;
static TCL_OBJCMDPROC_T PermObjCmd;
static TCL_OBJCMDPROC_T AddUserObjCmd;
static TCL_OBJCMDPROC_T DelUserObjCmd;
static TCL_OBJCMDPROC_T AddGroupObjCmd;
static TCL_OBJCMDPROC_T DelGroupObjCmd;
static TCL_OBJCMDPROC_T ListUsersObjCmd;
static TCL_OBJCMDPROC_T ListGroupsObjCmd;
static TCL_OBJCMDPROC_T ListPermsObjCmd;
static TCL_OBJCMDPROC_T DelPermObjCmd;
static TCL_OBJCMDPROC_T CheckPassObjCmd;
static TCL_OBJCMDPROC_T SetPassObjCmd;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;

static int AllowDenyObjCmd(
    ClientData data,
    Tcl_Interp *interp,
    TCL_SIZE_T objc,
    Tcl_Obj *const* objv,
    bool allow,
    bool user
);

static Ns_AuthorizeRequestProc AuthorizeRequestProc;
static Ns_AuthorizeUserProc AuthorizeUserProc;

static bool ValidateUserAddr(User *userPtr, const char *peer);
static void WalkCallback(Tcl_DString *dsPtr, const void *arg);
static Ns_ReturnCode CreateNonce(const char *privatekey, char **nonce, const char *uri);
static Ns_ReturnCode CreateHeader(const PServer *psevPtr, const Ns_Conn *conn, bool stale);
/*static Ns_ReturnCode CheckNonce(const char *privatekey, char *nonce, char *uri, int timeout);*/

static void FreeUserInfo(User *userPtr, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * Static variables defined in this file.
 */

static int uskey = -1;
static char usdigest[128];
static Tcl_HashTable serversTable;


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Initialize the perms module
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Init hash table, add Tcl commands.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *UNUSED(module))
{
    PServer       *psrvPtr;
    Tcl_HashEntry *hPtr;
    int            isNew;
    Ns_ReturnCode  result;

    if (uskey < 0) {
        uskey = Ns_UrlSpecificAlloc();
        Tcl_InitHashTable(&serversTable, TCL_ONE_WORD_KEYS);

        /*
         * Initialize global variable "usdigest" used in CreateNonce()
         */
        {
            double        d;
            char          buf[TCL_INTEGER_SPACE];
            Ns_CtxMD5     md5;
            unsigned long bigRamdomNumber;
            unsigned char sig[16];

            /* Make a really big random number */
            d = Ns_DRand();
            bigRamdomNumber = (unsigned long) (d * 1024 * 1024 * 1024);

            /* There is no requirement to hash it but it won't hurt */
            Ns_CtxMD5Init(&md5);
            snprintf(buf, sizeof(buf), "%lu", bigRamdomNumber);
            Ns_CtxMD5Update(&md5, (unsigned char *) buf, strlen(buf));
            Ns_CtxMD5Final(&md5, sig);
            Ns_HexString(sig, usdigest, 16, NS_TRUE);
        }
    }
    if (server == NULL) {
        Ns_Log(Warning, "nsperm: global module registration not supported,"
               " module must be registered on a server");
        result = NS_ERROR;
    } else {
        psrvPtr = ns_malloc(sizeof(PServer));
        psrvPtr->server = server;
        psrvPtr->servPtr = server != NULL ? Ns_GetServer(server) : NULL;
        Tcl_InitHashTable(&psrvPtr->users, TCL_STRING_KEYS);
        Tcl_InitHashTable(&psrvPtr->groups, TCL_STRING_KEYS);
        Ns_RWLockInit(&psrvPtr->lock);
        Ns_RWLockSetName2(&psrvPtr->lock, "rw:nsperm", server);
        Ns_RegisterAuthorizeRequest(server, AuthorizeRequestProc, NULL, "nsperm", NS_TRUE);
        Ns_RegisterAuthorizeUser(server, AuthorizeUserProc, NULL, "nsperm", NS_TRUE);

        result = Ns_TclRegisterTrace(server, AddCmds, psrvPtr, NS_TCL_TRACE_CREATE);
        hPtr = Tcl_CreateHashEntry(&serversTable, psrvPtr->servPtr, &isNew);
        Tcl_SetHashValue(hPtr, psrvPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AddCmds --
 *
 *      Add Tcl commands for perms
 *
 * Results:
 *      TCL_OK
 *
 * Side effects:
 *      Adds Tcl commands
 *
 *----------------------------------------------------------------------
 */

static int AddCmds(Tcl_Interp *interp, const void *arg)
{
    TCL_CREATEOBJCOMMAND(interp, "ns_perm", PermObjCmd, (ClientData)arg, NULL);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PermObjCmd --
 *
 *      Implements "ns_perm".
 *
 * Results:
 *      Std Tcl ret val
 *
 * Side effects:
 *      Yes.
 *
 *----------------------------------------------------------------------
 */

static int PermObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer *psrvPtr = data;
    int opt, status = TCL_OK;

    static const char *opts[] = {
        "adduser", "addgroup",
        "listusers", "listgroups", "listperms",
        "allowuser", "allowgroup",
        "denyuser", "denygroup",
        "checkpass", "setpass",
        "deluser", "delgroup", "delperm",
        NULL
    };
    enum {
        cmdAddUser, cmdAddGroup,
        cmdListUsers, cmdListGroups, cmdListPerms,
        cmdAllowUser, cmdAllowGroup,
        cmdDenyUser, cmdDenyGroup,
        cmdCheckPass, cmdSetPass,
        cmdDelUser, cmdDelGroup, cmdDelPerm
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/subcommand/ ?/arg .../?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "subcommand", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case cmdAddUser:
        status = AddUserObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdDelUser:
        status = DelUserObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdAddGroup:
        status = AddGroupObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdDelGroup:
        status = DelGroupObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdListUsers:
        status = ListUsersObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdListGroups:
        status = ListGroupsObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdListPerms:
        status = ListPermsObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdDelPerm:
        status = DelPermObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdAllowUser:
        status = AllowDenyObjCmd(psrvPtr, interp, objc, objv, NS_TRUE, NS_TRUE);
        break;

    case cmdDenyUser:
        status = AllowDenyObjCmd(psrvPtr, interp, objc, objv, NS_FALSE, NS_TRUE);
        break;

    case cmdAllowGroup:
        status = AllowDenyObjCmd(psrvPtr, interp, objc, objv, NS_TRUE, NS_FALSE);
        break;

    case cmdDenyGroup:
        status = AllowDenyObjCmd(psrvPtr, interp, objc, objv, NS_FALSE, NS_FALSE);
        break;

    case cmdCheckPass:
        status = CheckPassObjCmd(psrvPtr, interp, objc, objv);
        break;

    case cmdSetPass:
        status = SetPassObjCmd(psrvPtr, interp, objc, objv);
        break;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * CheckPassword --
 *
 *      Validate a supplied password against a stored password, handling
 *      either clear-text or encrypted storage formats.
 *
 * Parameters:
 *      inputPwd   - NUL-terminated string of the password provided by the user
 *      storedPwd  - NUL-terminated string of the password stored on the server
 *                   (either clear text or encrypted)
 *      flags      - bitmask indicating storage format; if USER_CLEAR_TEXT is
 *                   set, storedPwd is plain text, otherwise it is an encrypted hash
 *
 * Returns:
 *      NS_OK          if the provided password matches the stored password
 *      NS_UNAUTHORIZED if the passwords do not match
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
CheckPassword(const char *inputPwd, const char *storedPwd, int flags)
{
    Ns_ReturnCode result = NS_OK;
    /*
     * Handle the case where the password is stored as clear text.
     */
    if ((flags & USER_CLEAR_TEXT) != 0u) {
        if (!STREQ(inputPwd, storedPwd)) {
            result = NS_UNAUTHORIZED;
        }
    } else {
        char buf[NS_ENCRYPT_BUFSIZE];
        /*
         * Handle the case where the password is (en)crypted
         */
        Ns_Encrypt(inputPwd, storedPwd, buf);
        if (!STREQ(storedPwd, buf)) {
            result = NS_UNAUTHORIZED;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * GetServer --
 *
 *      Lookup the PServer instance corresponding to a given Ns_Server
 *      pointer in the serversTable.  This bridges from the opaque
 *      Ns_Server to the module’s private PServer struct.
 *
 * Parameters:
 *      servPtr    - pointer to an Ns_Server object (as passed by the core)
 *
 * Returns:
 *      pointer to the matching PServer, or NULL if not found
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static PServer *
GetServer(const Ns_Server *servPtr) {
    Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&serversTable, servPtr);
    if (hPtr == NULL) {
        return NULL;
    }
    return Tcl_GetHashValue(hPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * AuthorizeUserProc --
 *
 *      Authorize a user based on the nsperm records. This callback is called
 *      when just a user is to be authenticated (e.g. nscp module).
 *
 *      The function looks up the given username in the the nsperm records and
 *      verifies the password via CheckPassword().  When a record for this
 *      user exists, it sets *continuationPtr to TCL_BREAK to halt any further
 *      user callbacks.
 *
 * Parameters:
 *      arg             - unused (used e.g. for script level callbacks)
 *      servPtr         - pointer to the Ns_Server instance
 *      user            - username string to authenticate
 *      passwd          - password string provided by the client
 *      continuationPtr - out‐parameter; will be set to TCL_BREAK to stop
 *                        the callback chain after this proc
 *
 * Returns:
 *      NS_OK           - the user was found and the password matches
 *      NS_FORBIDDEN    - the user was found but the password is incorrect
 *      NS_UNAUTHORIZED - the server context is missing or the user is not found
 *
 * Side Effects:
 *      Acquires the server’s request‐auth rwlock in read mode.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
AuthorizeUserProc(void *UNUSED(arg), const Ns_Server *servPtr, const char *user, const char *passwd,
                  int *continuationPtr)
{
    Ns_ReturnCode  result = NS_OK;
    Tcl_HashEntry *hPtr;
    PServer       *psrvPtr;

    psrvPtr = GetServer(servPtr);
    /*fprintf(stderr, "NSPERM AuthorizeUserProc servPtr %p psrvPtr %p (user %s pass %s)\n",
      (void*)servPtr, (void*)psrvPtr, user, passwd);*/
    Ns_RWLockRdLock(&psrvPtr->lock);
    if (psrvPtr == NULL) {
        result = NS_UNAUTHORIZED;
    } else {
        hPtr = Tcl_FindHashEntry(&psrvPtr->users, user);
        if (hPtr != NULL) {
            User *userPtr;

            /*fprintf(stderr, "NSPERM ... user %s found\n", user);*/
            userPtr = Tcl_GetHashValue(hPtr);

            if (CheckPassword(passwd, userPtr->pwd, userPtr->flags) != NS_OK) {
                /*
                 * Incorrect password
                 */
                result = NS_FORBIDDEN;
            }
            *continuationPtr = TCL_BREAK;
        } else {
            result = NS_UNAUTHORIZED;
        }
    }
    Ns_RWLockUnlock(&psrvPtr->lock);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AuthorizeRequestProc --
 *
 *      Authorize a URL based on the nsperm records. This callback is called
 *      for incoming HTTP requests before serving a page.
 *
 *      Implemented logic:
 *
 *        1. Look up the Perm record for (method, URL). If none exists,
 *           return NS_UNAUTHORIZED (to trigger a WWW-Authenticate).
 *
 *        2. Extract "authmethod" from conn->auth (defaulting to "Basic").
 *
 *        3. Verify credentials:
 *             - Basic:     compare with userPtr->pwd via CheckPassword()
 *             - Digest:    call Ns_AuthDigestValidate()
 *           If the user record is missing or the check fails, return
 *           NS_UNAUTHORIZED (to allow the client to retry).
 *
 *        4. Enforce host/IP filters via ValidateUserAddr():
 *           on failure return NS_FORBIDDEN (and set *continuationPtr=TCL_BREAK).
 *
 *        5. Check deny lists:
 *             - if user ∈ denyuser      ⇒ NS_FORBIDDEN
 *             - if user ∈ any denygroup ⇒ NS_FORBIDDEN
 *
 *        6. Check allow lists:
 *             - if user ∈ allowuser   ⇒ NS_OK
 *             - if user ∈ any allowgroup ⇒ NS_OK
 *
 *        7. If no explicit allow and PERM_IMPLICIT_ALLOW flag is *not* set,
 *           return NS_UNAUTHORIZED; otherwise NS_OK.
 *
 *      On NS_UNAUTHORIZED with Digest, a WWW-Authenticate header is added.
 *
 * Results:
 *      NS_OK           - access granted
 *      NS_FORBIDDEN    - access denied, no retry (e.g. IP or deny rule)
 *      NS_UNAUTHORIZED - authentication required or failed, retry possible
 *      NS_ERROR        - internal error (e.g. no server or module)
 *
 *      NS_OK:
 *       - we have a permission record for this URL
 *         && user exists
 *         && credentials validated
 *         && address check OK
 *         && no explicit denial
 *         && explicit allow or implicit-allow
 *
 *      NS_FORBIDDEN is returned, when
 *       - Client IP fails address‐ACL check
 *       - User is explicitly denied
 *
 *      NS_UNAUTHORIZED is returned, when
 *       - User lookup failure
 *       - Unknown auth scheme
 *       - Basic auth, missing or wrong password
 *       - Digest auth, missing response or validation failure
 *       - Invalid client IP for anonymous (EMPTY) user
 *       - Not in any "allow" list and no implicit‐allow flag
 *
 *      NS_ERROR:
 *       - the function is called without a connection
 *       - the function is called without a registered server, no permission
 *         table available (missing registration?)  (IMPLICIT FORBIDDEN, retry
 *         won't change this)
 *
 * Side effects:
 *      Acquires the server’s request‐auth rwlock in read mode.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
AuthorizeRequestProc(void *UNUSED(arg), const Ns_Server *servPtr, const char *method, const char *url,
                     const char *user, const char *pwd, const char *peer,
                     int *continuationPtr)
{
    Ns_ReturnCode  status;
    const Ns_Set  *set;
    PServer       *psrvPtr;
    Perm          *permPtr;
    User          *userPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    const char    *group, *auth = NULL;
    const Ns_Conn *conn = Ns_GetConn();

    if (conn == NULL) {
        Ns_Log(Error, "nsperm: AuthorizeRequestProc called without connection");
        return NS_ERROR;
    }
    if (user == NULL) {
        user = NS_EMPTY_STRING;
    }
    if (pwd == NULL) {
        pwd = NS_EMPTY_STRING;
    }

    psrvPtr = GetServer(servPtr);
    if (psrvPtr == NULL) {
        *continuationPtr = TCL_ERROR;
        return NS_ERROR;
    }

    Ns_RWLockRdLock(&psrvPtr->lock);
    permPtr = Ns_UrlSpecificGet(Ns_ConnServPtr(conn), method, url, uskey,
                                0u, NS_URLSPACE_DEFAULT, NULL, NULL, NULL);

    /*fprintf(stderr, "NSPERM Ns_UrlSpecificGet for %s %s -> %p\n",
      method, url, (void*)permPtr);*/

    if (permPtr == NULL) {
        status = NS_OK;
        goto done;
    }

    /*
     * Make sure we have parsed the Authentication header properly, otherwise
     * fallback to Basic method.
     */

    set = Ns_ConnAuth(conn);
    if (set != NULL) {
        auth = Ns_SetIGet(set, "authmethod");
    }
    if (auth == NULL) {
        auth = "Basic";
    }

    /*
     * The first checks below deny access.
     */

    status = NS_UNAUTHORIZED;

    /*
     * Find user record, this is true for all methods
     */

    hPtr = Tcl_FindHashEntry(&psrvPtr->users, user);
    if (hPtr == NULL) {
        goto done;
    }
    /*fprintf(stderr, "... user %s found\n", user);*/

    userPtr = Tcl_GetHashValue(hPtr);

    /*
     * Check which auth method to use, permission record will
     * define how to verify the user.
     */

    /*
     * Note: the code handling "Basic" or "Digest" authentication should not
     * be here! The code for parsing the authorization string into the Ns_set
     * "connPtr->auth" is in auth.c (NsParseAuth); the block of user
     * authentication with Basic and Digest authentication shouldn be moved to
     * auth.c.
     */
    if (STREQ(auth, "Basic")) {

        /*
         * Basic Authentication: Verify user password (if any).
         */
        if (userPtr->pwd[0] != 0) {
            if (pwd[0] == 0) {
                goto done;

            } else if (CheckPassword(pwd, userPtr->pwd, userPtr->flags) != NS_OK) {
                /*
                 * Incorrect password
                 */
                goto done;
            }
        }
    } else if (STREQ(auth, "Digest")) {
        /*
         * Digest Authentication
         */
        if (userPtr->pwd[0] != 0) {
            if (pwd[0] != 0 && (userPtr->flags & USER_CLEAR_TEXT) == 0u) {
                /*
                 * Use the Digest Calculation to compute the hash based on a
                 * stored plain text password.
                 */
                if (Ns_AuthDigestValidate(set, userPtr->pwd) != NS_OK) {
                    goto done;
                }
            } else {
                goto done;
            }
        }
    } else {
        goto done;
    }

    /*
     * Check for a valid user address.
     */

    if (ValidateUserAddr(userPtr, peer) == NS_FALSE) {
        /*
         * Null user never gets forbidden--give a chance to enter password.
         */
    deny:
        if (*user != '\0') {
            status = NS_FORBIDDEN;
            *continuationPtr = TCL_BREAK;
        }
        goto done;
    }
    /*fprintf(stderr, "... address %s OK\n", peer);*/

    /*
     * Check user deny list.
     */

    if (Tcl_FindHashEntry(&permPtr->denyuser, user) != NULL) {
        goto deny;
    }
    /*fprintf(stderr, "... user not denied\n");*/

    /*
     * Loop over all groups in this perm record, and then
     * see if the user is in any of those groups.
     */

    hPtr = Tcl_FirstHashEntry(&permPtr->denygroup, &search);
    while (hPtr != NULL) {
        group = Tcl_GetHashKey(&permPtr->denygroup, hPtr);
        if (Tcl_FindHashEntry(&userPtr->groups, group) != NULL) {
            goto deny;
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    /*fprintf(stderr, "... user not in denygroup\n");*/

    /*
     * Valid checks below allow access.
     */

    status = NS_OK;

    /*
     * Check the allow lists, starting with users
     */

    if (Tcl_FindHashEntry(&permPtr->allowuser, user) != NULL) {
        goto done;
    }
    /*fprintf(stderr, "... user not in allowuser\n");*/

    /*
     * Loop over all groups in this perm record, and then
     * see if the user is in any of those groups.
     */

    hPtr = Tcl_FirstHashEntry(&permPtr->allowgroup, &search);
    while (hPtr != NULL) {
        group = Tcl_GetHashKey(&permPtr->allowgroup, hPtr);
        if (Tcl_FindHashEntry(&userPtr->groups, group) != NULL) {
            goto done;
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    /*fprintf(stderr, "... user not in allowgroup\n");*/

    /*
     * Checks above failed.  If implicit allow is not set,
     * change the status back to unauthorized. This flag will be set only when
     * at least one deny user was added to the permission record, otherwise
     * it will allow user with name "" to pass. What a nonsense!
     */

    if (!(permPtr->flags & PERM_IMPLICIT_ALLOW)) {
        status = NS_UNAUTHORIZED;
    }

  done:

    /*
     * For Digest authentication we create WWW-Authenticate header manually
     */

    if (status == NS_UNAUTHORIZED && auth != NULL && !strcmp(auth, "Digest")) {
        CreateHeader(psrvPtr, conn, NS_FALSE);
    }

    Ns_RWLockUnlock(&psrvPtr->lock);
    /*fprintf(stderr, "NSPERM AuthorizeRequestProc returns %d (%s)\n", status, Ns_ReturnCodeString(status));*/
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ValidateUserAddr --
 *
 *      Determine whether a client's peer address is permitted for a given user,
 *      based on the user's configured IP/netmask and hostname filters.
 *
 *      1. If no peer string is provided, allow by default.
 *      2. Parse the peer string into a sockaddr.  If parsing fails, deny.
 *      3. For each network mask in userPtr->masks:
 *         - Mask the peer address with this mask.
 *         - Look up the masked address in userPtr->nets.
 *         - If found, compare the stored mask string; if they match:
 *             -> Return true if USER_FILTER_ALLOW is set, false otherwise.
 *      4. If no mask matched:
 *         - Start with the inverted USER_FILTER_ALLOW flag as the default result.
 *      5. If the user has host-based filters (userPtr->hosts):
 *         - Perform a reverse DNS lookup of the peer address.
 *         - Slide through each hostname suffix (e.g. "foo.example.com", ".example.com", ".com"):
 *             -> If any suffix is in userPtr->hosts, return allow or deny per USER_FILTER_ALLOW.
 *
 *      USER_FILTER_ALLOW is set, when the user was added with the -allow flag,
 *      meaning the the user is granted access only on the specified hosts.
 *
 * Parameters:
 *      userPtr  - pointer to the User structure containing masks, nets, hosts, and flags.
 *      peer     - NUL-terminated string of the client’s IP address.
 *
 * Returns:
 *      true  if the peer passes the user’s address/host filters (allowed),
 *      false if it fails any check (denied).
 *
 * Side Effects:
 *      May perform reverse DNS lookups via Ns_GetHostByAddr().
 *
 *----------------------------------------------------------------------
 */
static bool
ValidateUserAddr(User *userPtr, const char *peer)
{
    int                    validIp;
    bool                   success;
    Tcl_HashSearch         search;
    Tcl_HashEntry         *hPtr;
    struct NS_SOCKADDR_STORAGE  peerStruct, ipStruct;
    struct sockaddr       *peerPtr = (struct sockaddr *)&peerStruct,
                          *ipPtr = (struct sockaddr *)&ipStruct;

    if (peer == NULL) {
        return NS_TRUE;
    }

    memset(peerPtr, 0, sizeof(struct NS_SOCKADDR_STORAGE));

    validIp = ns_inet_pton(peerPtr, peer);
    if (validIp < 1) {
        return NS_FALSE;
    }

    /*
     * Loop over each netmask, AND the peer address with it,
     * then see if that address is in the list.
     */

    hPtr = Tcl_FirstHashEntry(&userPtr->masks, &search);
    while (hPtr != NULL) {
        const struct sockaddr *maskPtr;
        Tcl_HashEntry *entryPtr;

        maskPtr = (struct sockaddr *)Tcl_GetHashKey(&userPtr->masks, hPtr);

        Ns_SockaddrMask(peerPtr, maskPtr, ipPtr);
        /*
          Ns_LogSockaddr(Notice, "FOUND: mask", maskPtr);
          Ns_LogSockaddr(Notice, "FOUND: peer", peerPtr);
          Ns_LogSockaddr(Notice, "FOUND: ====", ipPtr);
        */

        /*
         * There is a potential match. Now make sure it works with the
         * right address's mask.
         */
        entryPtr = Tcl_FindHashEntry(&userPtr->nets, (char *)ipPtr);

        if (entryPtr != NULL) {
            char maskString[NS_IPADDR_SIZE];

            ns_inet_ntop(maskPtr, maskString, NS_IPADDR_SIZE);
            /*
             * We have an entry, does it really match with saved mask?
             */
            if (STREQ((char *)Tcl_GetHashValue(entryPtr), maskString)) {
                if (userPtr->flags & USER_FILTER_ALLOW) {
                    success = NS_TRUE;
                } else {
                    success = NS_FALSE;
                }
                return success;
            }
        }
        hPtr = Tcl_NextHashEntry(&search);
    }

    if (userPtr->flags & USER_FILTER_ALLOW) {
        success = NS_FALSE;
    } else {
        success = NS_TRUE;
    }
    if (userPtr->hosts.numEntries > 0) {
        Tcl_DString addr;

        /*
         * If we have gotten this far, it is necessary to do a
         * reverse dns lookup and try to make a decision
         * based on that, if possible.
         */

        Tcl_DStringInit(&addr);
        if (Ns_GetHostByAddr(&addr, peer) == NS_TRUE) {
            char *start = addr.string;

            /*
             * If the hostname is blah.aol.com, check the hash table
             * for:
             *
             * blah.aol.com
             * .aol.com
             * .com
             *
             * Break out of the loop as soon as a match is found or
             * all possibilities are exhausted.
             */

            while (start != NULL && start[0] != '\0') {
                char *last;

                last = start;
                hPtr = Tcl_FindHashEntry(&userPtr->hosts, start);
                if (hPtr != NULL) {
                    if (userPtr->flags & USER_FILTER_ALLOW) {
                        success = NS_TRUE;
                    } else {
                        success = NS_FALSE;
                    }
                    break;
                }
                start = strchr(start + 1, INTCHAR('.'));
                if (start == NULL) {
                    break;
                }
                if (last == start) {
                    Ns_Log(Warning, "nsperm: " "invalid hostname '%s'", addr.string);
                    break;
                }
            }
        }
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeUserInfo --
 *
 *      Free the user information.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      free memory.
 *
 *----------------------------------------------------------------------
 */

static void
FreeUserInfo(User *userPtr, const char *name)
{
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    NS_NONNULL_ASSERT(userPtr != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    hPtr = Tcl_FirstHashEntry(&userPtr->groups, &search);
    while (hPtr != NULL) {
        Group *groupPtr = Tcl_GetHashValue(hPtr);

        hPtr = Tcl_FindHashEntry(&groupPtr->users, name);
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    hPtr = Tcl_FirstHashEntry(&userPtr->nets, &search);
    while (hPtr != NULL) {
        char *maskString = Tcl_GetHashValue(hPtr);

        ns_free(maskString);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&userPtr->groups);
    Tcl_DeleteHashTable(&userPtr->masks);
    Tcl_DeleteHashTable(&userPtr->nets);
    Tcl_DeleteHashTable(&userPtr->hosts);
    ns_free(userPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * AddUserObjCmd --
 *
 *      Implements "ns_perm adduser".
 *
 * Results:
 *      Tcl result
 *
 * Side effects:
 *      A user may be added to the global user hash table
 *
 *----------------------------------------------------------------------
 */

static int AddUserObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer            *psrvPtr = data;
    User               *userPtr;
    Tcl_HashEntry      *hPtr;
    struct NS_SOCKADDR_STORAGE ip, mask;
    struct sockaddr    *ipPtr = (struct sockaddr *)&ip, *maskPtr = (struct sockaddr *)&mask;
    char                buf[NS_ENCRYPT_BUFSIZE];
    char               *name, *pwd, *field = NULL, *salt = NULL;
    int                 isNew, allow = 0, deny = 0, clear = 0;
    TCL_SIZE_T          nargs = 0, i;

    Ns_ObjvSpec opts[] = {
        {"-allow", Ns_ObjvBool, &allow, INT2PTR(NS_TRUE)},
        {"-deny", Ns_ObjvBool, &deny, INT2PTR(NS_TRUE)},
        {"-clear", Ns_ObjvBool, &clear, INT2PTR(NS_TRUE)},
        {"-salt", Ns_ObjvString, &salt, NULL},
        {"--", Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {"encpass", Ns_ObjvString, &pwd, NULL},
        {"userfield", Ns_ObjvString, &field, NULL},
        {"?host", Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    userPtr = ns_calloc(1u, sizeof(User));
    if (clear != 0) {
        userPtr->flags |= USER_CLEAR_TEXT;
    }
    if (salt != NULL) {
        Ns_Encrypt(pwd, salt, buf);
        pwd = buf;
        userPtr->flags &= ~USER_CLEAR_TEXT;
    }
    snprintf(userPtr->pwd, sizeof(userPtr->pwd), "%s", pwd);

    Tcl_InitHashTable(&userPtr->nets, (int)(sizeof(struct NS_SOCKADDR_STORAGE) / sizeof(int)));
    Tcl_InitHashTable(&userPtr->masks, (int)(sizeof(struct NS_SOCKADDR_STORAGE) / sizeof(int)));
    Tcl_InitHashTable(&userPtr->hosts, TCL_STRING_KEYS);
    Tcl_InitHashTable(&userPtr->groups, TCL_STRING_KEYS);

    // fprintf(stderr, "============= add user <%s> pwd <%s> field <%s> nrags %d\n", name, pwd, field, nargs);

    /*
     * Both -allow and -deny can be used for consistency, but
     * -deny has precedence
     */

    if (allow && !deny) {
        userPtr->flags |= USER_FILTER_ALLOW;
    }

    /*
     * Loop over each parameter and figure out what it is. The
     * possibilities are ipaddr/netmask, hostname, or partial hostname:
     * 192.168.2.3/255.255.255.0, foo.bar.com, or .bar.com
     */

    for (i = (TCL_SIZE_T)objc - nargs; i < (TCL_SIZE_T)objc; ++i) {
        Ns_ReturnCode status;
        char         *net = Tcl_GetString(objv[i]);

        status = Ns_SockaddrParseIPMask(interp, net, ipPtr, maskPtr, NULL);
        if (status != NS_OK) {
            goto fail;
        }

        /*
         * Is this a new netmask? If so, add it to the list.
         * A list of netmasks is maintained and every time a
         * new connection comes in, the peer address is ANDed with
         * each of them and a lookup on that address is done
         * on the hash table of networks.
         */
        (void) Tcl_CreateHashEntry(&userPtr->masks, (char *)maskPtr, &isNew);

        /*
         * Add the potentially masked IpAddress to the nets table.
         */
        hPtr = Tcl_CreateHashEntry(&userPtr->nets,  (char *)ipPtr, &isNew);
        if (hPtr != NULL) {
            char ipString[NS_IPADDR_SIZE];
            Tcl_SetHashValue(hPtr,
                             (ClientData)ns_strdup(ns_inet_ntop(maskPtr, ipString, sizeof(ipString))));
        }
        if (isNew == 0) {
            Ns_TclPrintfResult(interp, "duplicate entry: %s", net);
            goto fail;
        }
    }

    /*
     * Add the user.
     */

    Ns_RWLockWrLock(&psrvPtr->lock);
    hPtr = Tcl_CreateHashEntry(&psrvPtr->users, name, &isNew);
    if (isNew == 0) {
        Ns_TclPrintfResult(interp, "duplicate user: %s", name);
        goto fail0;
    }
    Tcl_SetHashValue(hPtr, userPtr);
    Ns_RWLockUnlock(&psrvPtr->lock);
    return TCL_OK;

  fail0:
    Ns_RWLockUnlock(&psrvPtr->lock);

  fail:
    FreeUserInfo(userPtr, name);

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * DelUserObjCmd --
 *
 *      Implements "ns_perm deluser".
 *
 * Results:
 *      Tcl result
 *
 * Side effects:
 *      A user may be deleted from the global user hash table
 *
 *----------------------------------------------------------------------
 */

static int DelUserObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer       *psrvPtr = data;
    char          *name = NULL;
    User          *userPtr = NULL;
    Tcl_HashEntry *hPtr;

    Ns_ObjvSpec args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    Ns_RWLockWrLock(&psrvPtr->lock);
    hPtr = Tcl_FindHashEntry(&psrvPtr->users, name);
    if (hPtr != NULL) {
        userPtr = Tcl_GetHashValue(hPtr);
        Tcl_DeleteHashEntry(hPtr);
    }
    Ns_RWLockUnlock(&psrvPtr->lock);

    if (userPtr != NULL) {
        FreeUserInfo(userPtr, name);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ListUsersObjCmd --
 *
 *      Implements "ns_perm listusers".
 *
 * Results:
 *      Tcl result
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int ListUsersObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 2, objv, NULL);
        result = TCL_ERROR;
    } else {
        PServer        *psrvPtr = data;
        Tcl_HashSearch  search, msearch;
        Tcl_HashEntry  *hPtr;
        Tcl_DString     ds;

        Tcl_DStringInit(&ds);
        Ns_RWLockRdLock(&psrvPtr->lock);
        hPtr = Tcl_FirstHashEntry(&psrvPtr->users, &search);

        while (hPtr != NULL) {
            char              ipString[NS_IPADDR_SIZE];
            User             *userPtr = Tcl_GetHashValue(hPtr);
            Tcl_HashEntry    *mPtr;
            struct sockaddr  *netPtr;

            Ns_DStringPrintf(&ds, "{%s} {%s} {",
                             (const char*)Tcl_GetHashKey(&psrvPtr->users, hPtr),
                             userPtr->pwd);

            if (userPtr->hosts.numEntries > 0 || userPtr->masks.numEntries > 0 || userPtr->nets.numEntries > 0) {
                Ns_DStringPrintf(&ds, " %s ", ((userPtr->flags & USER_FILTER_ALLOW) != 0u) ? "-allow" : "-deny");
            }
            /*
             * Append all values from networks
             */
            mPtr = Tcl_FirstHashEntry(&userPtr->nets, &msearch);
            while (mPtr != NULL) {
                netPtr = (struct sockaddr *)Tcl_GetHashKey(&userPtr->nets, hPtr);
                Ns_DStringPrintf(&ds, "%s ", ns_inet_ntop(netPtr, ipString, sizeof(ipString)));
                mPtr = Tcl_NextHashEntry(&msearch);
            }

            /*
             * Append all values from masks
             */
            mPtr = Tcl_FirstHashEntry(&userPtr->masks, &msearch);
            while (mPtr != NULL) {
                netPtr = (struct sockaddr *)Tcl_GetHashKey(&userPtr->nets, hPtr);
                Ns_DStringPrintf(&ds, "%s ", ns_inet_ntop(netPtr, ipString, sizeof(ipString)));
                mPtr = Tcl_NextHashEntry(&msearch);
            }

            /*
             * Append all values from hosts
             */
            mPtr = Tcl_FirstHashEntry(&userPtr->hosts, &msearch);
            while (mPtr != NULL) {
                Ns_DStringPrintf(&ds, "%s ", (const char*)Tcl_GetHashKey(&userPtr->hosts, mPtr));
                mPtr = Tcl_NextHashEntry(&msearch);
            }
            Tcl_DStringAppend(&ds, "} ", 2);

            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_RWLockUnlock(&psrvPtr->lock);
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AddGroupObjCmd --
 *
 *      Implements "ns_perm addgroup". Adds a group to the global groups list.
 *
 * Results:
 *      Standard Tcl
 *
 * Side effects:
 *      A group will be created
 *
 *----------------------------------------------------------------------
 */

static int AddGroupObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer       *psrvPtr = data;
    char          *name, *user;
    User          *userPtr;
    Group         *groupPtr;
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;
    int            isNew;
    TCL_SIZE_T     param;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "/group/ /user/ ?/user .../?");
        return TCL_ERROR;
    }

    /*
     * Create & populate the structure for a new group.
     */

    name = Tcl_GetString(objv[2]);
    groupPtr = ns_malloc(sizeof(Group));
    Tcl_InitHashTable(&groupPtr->users, TCL_STRING_KEYS);

    /*
     * Loop over each of the users who is to be in the group, make sure
     * it is ok, and add this user. Also put the group into the user's list
     * of groups he's in.
     */

    for (param = 3; param < objc; param++) {
        user = Tcl_GetString(objv[param]);
        hPtr = Tcl_FindHashEntry(&psrvPtr->users, user);
        if (hPtr == NULL) {
            Ns_TclPrintfResult(interp, "no such user: %s", user);
            goto fail;
        }
        userPtr = Tcl_GetHashValue(hPtr);

        /*
         * Add the user to the group's list of users
         */

        hPtr = Tcl_CreateHashEntry(&groupPtr->users, user, &isNew);
        if (isNew == 0) {
          dupuser:
            Ns_TclPrintfResult(interp, "user \"%s\" already in group \"%s\"", user, name);
            goto fail;
        }
        Tcl_SetHashValue(hPtr, userPtr);

        /*
         * Add the group to the user's list of groups
         */

        hPtr = Tcl_CreateHashEntry(&userPtr->groups, name, &isNew);
        if (isNew == 0) {
            goto dupuser;
        }
        Tcl_SetHashValue(hPtr, groupPtr);
    }

    /*
     * Add the group to the global list of groups
     */

    Ns_RWLockWrLock(&psrvPtr->lock);
    hPtr = Tcl_CreateHashEntry(&psrvPtr->groups, name, &isNew);
    if (isNew == 0) {
        Ns_TclPrintfResult(interp, "duplicate group: %s", name);
        goto fail0;
    }
    Tcl_SetHashValue(hPtr, groupPtr);
    Ns_RWLockUnlock(&psrvPtr->lock);
    return TCL_OK;

  fail0:
    Ns_RWLockUnlock(&psrvPtr->lock);

  fail:
    hPtr = Tcl_FirstHashEntry(&groupPtr->users, &search);
    while (hPtr != NULL) {
        userPtr = Tcl_GetHashValue(hPtr);
        hPtr = Tcl_FindHashEntry(&userPtr->groups, name);
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&groupPtr->users);
    ns_free(groupPtr);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * DelGroupObjCmd --
 *
 *      Implements "ns_perm delgroup".
 *
 * Results:
 *      Tcl result
 *
 * Side effects:
 *      A group may be deleted from the global user hash table
 *
 *----------------------------------------------------------------------
 */

static int DelGroupObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer *psrvPtr = data;
    char *name = NULL;
    User *userPtr;
    Group *groupPtr = NULL;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    Ns_ObjvSpec args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_RWLockWrLock(&psrvPtr->lock);
    hPtr = Tcl_FindHashEntry(&psrvPtr->groups, name);
    if (hPtr != NULL) {
        groupPtr = Tcl_GetHashValue(hPtr);
        Tcl_DeleteHashEntry(hPtr);
    }
    Ns_RWLockUnlock(&psrvPtr->lock);

    if (groupPtr != NULL) {
        hPtr = Tcl_FirstHashEntry(&groupPtr->users, &search);
        while (hPtr != NULL) {
            userPtr = Tcl_GetHashValue(hPtr);
            hPtr = Tcl_FindHashEntry(&userPtr->groups, name);
            if (hPtr != NULL) {
                Tcl_DeleteHashEntry(hPtr);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&groupPtr->users);
        ns_free(groupPtr);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ListGroupsObjCmd --
 *
 *      Implements "ns_perm listgroups".
 *
 * Results:
 *      Tcl result
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int ListGroupsObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 2, objv, NULL);
        result = TCL_ERROR;

    } else {
        PServer        *psrvPtr = data;
        Tcl_HashSearch  search;
        Tcl_HashEntry  *hPtr;
        Tcl_DString     ds;

        Tcl_DStringInit(&ds);
        Ns_RWLockRdLock(&psrvPtr->lock);
        hPtr = Tcl_FirstHashEntry(&psrvPtr->groups, &search);
        while (hPtr != NULL) {
            Tcl_HashSearch usearch;
            Tcl_HashEntry *uhPtr;
            Group         *groupPtr = Tcl_GetHashValue(hPtr);

            Ns_DStringPrintf(&ds, "%s { ",
                             (const char *)Tcl_GetHashKey(&psrvPtr->groups, hPtr));

            /*
             * All users for this group
             */

            uhPtr = Tcl_FirstHashEntry(&groupPtr->users, &usearch);
            while (uhPtr != NULL) {
                Ns_DStringPrintf(&ds, "\"%s\" ",
                                 (const char *)Tcl_GetHashKey(&groupPtr->users, uhPtr));
                uhPtr = Tcl_NextHashEntry(&usearch);
            }
            Tcl_DStringAppend(&ds, "} ", 2);

            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_RWLockUnlock(&psrvPtr->lock);
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AllowDenyObjCmd --
 *
 *      Implements:
 *
 *         "ns_perm allowuser"
 *         "ns_perm allowgroup"
 *         "ns_perm denyuser"
 *         "ns_perm denygroup"
 *
 *      Adds/removes a record that will allow or deny access to
 *      the specified URL.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      A perm record may be created
 *
 *----------------------------------------------------------------------
 */

static int AllowDenyObjCmd(
    ClientData data,
    Tcl_Interp *interp,
    TCL_SIZE_T objc,
    Tcl_Obj *const* objv,
    bool allow,
    bool user
) {
    char      *method = NULL, *url = NULL;
    int        noinherit = 0, result = TCL_OK;
    TCL_SIZE_T nargs = 0;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,   &noinherit,  INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec argsUser[] = {
        {"method", Ns_ObjvString, &method, NULL},
        {"url",    Ns_ObjvString, &url, NULL},
        {"user",   Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec argsGroup[] = {
        {"method", Ns_ObjvString, &method, NULL},
        {"url",    Ns_ObjvString, &url, NULL},
        {"group",  Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec *args = user ? argsUser : argsGroup;

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        PServer     *psrvPtr = data;
        Perm        *permPtr;
        Tcl_DString  base;
        int          isNew;
        TCL_SIZE_T   i;
        unsigned int flags = 0u;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }

        /*
         * Construct the base url.
         */

        Tcl_DStringInit(&base);
        Ns_NormalizeUrl(&base, url);

        /*
         * Locate and verify the exact record.
         */

        Ns_RWLockWrLock(&psrvPtr->lock);
        permPtr = Ns_UrlSpecificGet(psrvPtr->servPtr, method, url, uskey,
                                    0u, NS_URLSPACE_DEFAULT, NULL, NULL, NULL);

        if (permPtr != NULL && !STREQ(base.string, permPtr->baseurl)) {
            permPtr = NULL;
        }
        if (permPtr == NULL) {
            permPtr = ns_calloc(1u, sizeof(Perm));
            permPtr->baseurl = Ns_DStringExport(&base);
            Tcl_InitHashTable(&permPtr->allowuser, TCL_STRING_KEYS);
            Tcl_InitHashTable(&permPtr->denyuser, TCL_STRING_KEYS);
            Tcl_InitHashTable(&permPtr->allowgroup, TCL_STRING_KEYS);
            Tcl_InitHashTable(&permPtr->denygroup, TCL_STRING_KEYS);
            Ns_UrlSpecificSet(psrvPtr->server, method, url, uskey, permPtr, flags, NULL);
        }
        if (!allow) {
            permPtr->flags |= PERM_IMPLICIT_ALLOW;
        }

        for (i = (TCL_SIZE_T)objc - nargs; i < (TCL_SIZE_T)objc; i++) {
            char *key = Tcl_GetString(objv[i]);

            if (user) {
                if (allow) {
                    (void) Tcl_CreateHashEntry(&permPtr->allowuser, key, &isNew);
                } else {
                    (void) Tcl_CreateHashEntry(&permPtr->denyuser, key, &isNew);
                }
            } else {
                if (allow) {
                    (void) Tcl_CreateHashEntry(&permPtr->allowgroup, key, &isNew);
                } else {
                    (void) Tcl_CreateHashEntry(&permPtr->denygroup, key, &isNew);
                }
            }
        }
        Ns_RWLockUnlock(&psrvPtr->lock);
        Tcl_DStringFree(&base);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DelPermObjCmd --
 *
 *      Implements "ns_perm delperm". Removes permission record.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      A permission record may be deleted.
 *
 *----------------------------------------------------------------------
 */

static int DelPermObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer     *psrvPtr = data;
    Perm        *permPtr;
    Tcl_DString  base;
    char        *method, *url;
    int          noinherit = 0;
    unsigned int flags = NS_OP_RECURSE;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool, &noinherit, INT2PTR(NS_TRUE)},
        {"--", Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL, NULL, NULL}
    };

    Ns_ObjvSpec args[] = {
        {"method", Ns_ObjvString, &method, NULL},
        {"url", Ns_ObjvString, &url, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit != 0) {
        flags |= NS_OP_NOINHERIT;
    }

    /*
     * Construct the base url.
     */

    Tcl_DStringInit(&base);
    Ns_NormalizeUrl(&base, url);

    /*
     * Locate and verify the exact record.
     */

    Ns_RWLockWrLock(&psrvPtr->lock);
    permPtr = Ns_UrlSpecificGet(psrvPtr->servPtr, method, url, uskey,
                                0u, NS_URLSPACE_DEFAULT, NULL, NULL, NULL);
    if (permPtr != NULL) {
        Ns_UrlSpecificDestroy(psrvPtr->server, method, url, uskey, flags);
        ns_free(permPtr->baseurl);
        Tcl_DeleteHashTable(&permPtr->allowuser);
        Tcl_DeleteHashTable(&permPtr->denyuser);
        Tcl_DeleteHashTable(&permPtr->allowgroup);
        Tcl_DeleteHashTable(&permPtr->denygroup);
        ns_free(permPtr);
    }
    Ns_RWLockUnlock(&psrvPtr->lock);
    Tcl_DStringFree(&base);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ListPermsObjCmd --
 *
 *      Implements "ns_perm listperms".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int ListPermsObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 2, objv, NULL);
        result = TCL_ERROR;
    } else {
        PServer *psrvPtr = data;
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        Ns_RWLockRdLock(&psrvPtr->lock);
        Ns_UrlSpecificWalk(uskey, psrvPtr->server, WalkCallback, &ds);
        Ns_RWLockUnlock(&psrvPtr->lock);

        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

static void WalkCallback(Tcl_DString *dsPtr, const void *arg)
{
    Perm *permPtr = (Perm *)arg;
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;

    if (permPtr->flags & PERM_IMPLICIT_ALLOW) {
        Tcl_DStringAppend(dsPtr, " -implicitallow ", 16);
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->allowuser, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -allowuser {", Tcl_GetHashKey(&permPtr->allowuser, hPtr), "}", NS_SENTINEL);
        hPtr = Tcl_NextHashEntry(&search);
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->denyuser, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -denyuser {", Tcl_GetHashKey(&permPtr->denyuser, hPtr), "}", NS_SENTINEL);
        hPtr = Tcl_NextHashEntry(&search);
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->allowgroup, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -allowgroup {", Tcl_GetHashKey(&permPtr->allowgroup, hPtr), "}", NS_SENTINEL);
        hPtr = Tcl_NextHashEntry(&search);
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->denygroup, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -denygroup {", Tcl_GetHashKey(&permPtr->denygroup, hPtr), "}", NS_SENTINEL);
        hPtr = Tcl_NextHashEntry(&search);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CheckPassObjCmd --
 *
 *      Implements "ns_perm checkpass". Checks supplied user password against
 *      internal database.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int
CheckPassObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer *psrvPtr = data;
    int rc = TCL_ERROR;
    User *userPtr;
    char *user, *pwd;
    Tcl_HashEntry *hPtr;

    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "/user/ /passwd/");
        return TCL_ERROR;
    }
    user = Tcl_GetString(objv[2]);
    pwd = Tcl_GetString(objv[3]);

    Ns_RWLockRdLock(&psrvPtr->lock);
    hPtr = Tcl_FindHashEntry(&psrvPtr->users, user);
    if (hPtr == NULL) {
        Ns_TclPrintfResult(interp, "user not found");
        goto done;
    }
    userPtr = Tcl_GetHashValue(hPtr);
    if (userPtr->pwd[0] != 0) {
        if (pwd[0] == 0) {
            Ns_TclPrintfResult(interp, "empty password given");
            goto done;
        }

        // Use the CheckPassword function for validation
        if (CheckPassword(pwd, userPtr->pwd, userPtr->flags) != NS_OK) {
            Ns_TclPrintfResult(interp, "incorrect password");
            goto done;
        }
    }
    rc = TCL_OK;

  done:
    Ns_RWLockUnlock(&psrvPtr->lock);
    return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * SetPassObjCmd --
 *
 *      Implements "ns_perm setpass". Assigns new password to the user.
 *
 * Results:
 *      Standard Tcl result
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int
SetPassObjCmd(ClientData data, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    PServer       *psrvPtr = data;
    int            rc = 0;
    User          *userPtr;
    Tcl_HashEntry *hPtr;
    char          *user, *pwd, *salt;
    char           buf[NS_ENCRYPT_BUFSIZE];

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "/user/ /encpass/ ?/salt/?");
        return TCL_ERROR;
    }
    user = Tcl_GetString(objv[2]);
    pwd = Tcl_GetString(objv[3]);
    salt = objc > 4 ? Tcl_GetString(objv[4]) : NULL;

    Ns_RWLockRdLock(&psrvPtr->lock);
    hPtr = Tcl_FindHashEntry(&psrvPtr->users, user);
    if (hPtr == NULL) {
        goto done;
    }
    userPtr = Tcl_GetHashValue(hPtr);
    if (salt != NULL) {
        Ns_Encrypt(pwd, salt, buf);
        pwd = buf;
    }
    snprintf(userPtr->pwd, sizeof(userPtr->pwd), "%s", pwd);
    rc = 1;

  done:
    Ns_RWLockUnlock(&psrvPtr->lock);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateNonce --
 *
 *      Create the nonce to be used by the client to hash against.
 *      The hash is a uuencoded string that consists of:
 *
 *      time-stamp H(time-stamp ":" uri ":" private-key)
 *
 *      Note that this function is called here with uri = ""
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
*/

static Ns_ReturnCode
CreateNonce(const char *privatekey, char **nonce, const char *uri)
{
    Ns_ReturnCode status = NS_OK;

    if (privatekey == NULL) {
        status = NS_ERROR;
    } else {
        time_t        now;
        Tcl_DString   ds;
        Ns_CtxMD5     md5;
        unsigned char sig[16];
        char          buf[33];
        char          bufcoded[1 + (4 * 48) / 2];

        now = time(NULL);

        Tcl_DStringInit(&ds);
        Ns_DStringPrintf(&ds, "%" PRId64 ":%s:%s", (int64_t) now, uri, privatekey);

        Ns_CtxMD5Init(&md5);
        Ns_CtxMD5Update(&md5, (unsigned char *) ds.string, (unsigned int) ds.length);
        Ns_CtxMD5Final(&md5, sig);
        Ns_HexString(sig, buf, 16, NS_TRUE);

        /* encode the current time and MD5 string into the nonce */
        Tcl_DStringSetLength(&ds, 0);
        Ns_DStringPrintf(&ds, "%" PRId64 " %s", (int64_t) now, buf);
        Ns_HtuuEncode((unsigned char *) ds.string, (unsigned int) ds.length, bufcoded);

        *nonce = ns_strdup(bufcoded);
    }

    return status;
}


#if 0
/*
 * unused
 */
/*
 *----------------------------------------------------------------------
 *
 * CheckNonce --
 *
 *      Given a nonce value ensure that it hasn't been tampered with
 *      and that it isn't stale. The hash is a uuencoded string that
 *      consists of:
 *
 *      time-stamp H(time-stamp ":" uri ":" private-key)
 *
 * Results:
 *      NS_OK or NS_ERROR if the nonce is stale.
 *
 * Side effects:
 *      None.
 */
static Ns_ReturnCode
CheckNonce(const char *privatekey, char *nonce, char *uri, int timeout)
{
    Ns_CtxMD5 md5;
    Tcl_DString ds;
    char buf[33];
    char *decoded;
    char *ntime;
    char *tnonce;
    int n;
    unsigned char sig[16];
    time_t now, nonce_time;
    Ns_ReturnCode status = NS_OK;

    if (privatekey == NULL) {
        result = NS_ERROR;
    } else {

        time(&now);

        /* decode the nonce */
        n = 3 + ((strlen(nonce) * 3) / 4);
        decoded = ns_malloc((unsigned int) n);
        n = Ns_HtuuDecode(nonce, (unsigned char *) decoded, n);
        decoded[n] = '\0';

        ntime = ns_strtok(decoded, " ");
        tnonce = ns_strtok(NULL, " ");

        /* recreate the nonce to ensure that it isn't corrupted */
        Ns_CtxMD5Init(&md5);

        Tcl_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, ntime, ":", uri, ":", privatekey, NS_SENTINEL);

        Ns_CtxMD5Update(&md5, (unsigned char *) ds.string, (unsigned int) ds.length);
        Ns_CtxMD5Final(&md5, sig);
        Ns_HexString(sig, buf, 16, NS_TRUE);

        /* Check for a stale timestamp. If the timestamp is stale we still check
         * to see if the user sent the proper digest password. The stale flag
         * is only set if the nonce is expired AND the credentials are OK, otherwise
         * the get a 401, but that happens elsewhere.
         */

        nonce_time = (time_t) strtol(ntime, (char **) NULL, 10);

        if ((now - nonce_time) > timeout) {
            status = NS_ERROR;
        }

        if (!(STREQ(tnonce, buf))) {
            status = NS_ERROR;
        }
    }
    return status;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * CreateHeader --
 *
 *      Assigns WWW-Authenticate headers according to Digest
 *      authentication rules.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Appends HTTP headers to output headers
 *
 *----------------------------------------------------------------------
*/

static Ns_ReturnCode
CreateHeader(const PServer *psrvPtr, const Ns_Conn *conn, bool stale)
{
    Ns_ReturnCode  status = NS_OK;
    char          *nonce = 0;

    if (CreateNonce(usdigest, &nonce, NS_EMPTY_STRING) == NS_ERROR) {
        status = NS_ERROR;
    } else {
        Tcl_DString    ds;

        Tcl_DStringInit(&ds);
        Ns_DStringPrintf(&ds, "Digest realm=\"%s\", nonce=\"%s\", algorithm=\"MD5\", qop=\"auth\"", psrvPtr->server, nonce);

        if (stale) {
            Ns_DStringVarAppend(&ds, ", stale=\"true\"", NS_SENTINEL);
        }
        Ns_ConnSetHeadersSz(conn, "www-authenticate", 16, ds.string, ds.length);
    }
    return status;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
