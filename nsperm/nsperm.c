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
 * nsperm --
 *
 *	Permissions
 */

#include "ns.h"

#ifndef INADDR_NONE
#define INADDR_NONE (-1)
#endif

/*
 * The following flags are for user record
 */

#define USER_FILTER_ALLOW     1
#define USER_CLEAR_TEXT       2

/*
 * The following flags are for permission record
 */

#define PERM_IMPLICIT_ALLOW   1


NS_EXPORT const int Ns_ModuleVersion = 1;

/*
 * The following structure is allocated for each instance of the module.
 */

typedef struct Server {
    char *server;
    Tcl_HashTable users;
    Tcl_HashTable groups;
    Ns_RWLock lock;
} Server;

/*
 * The "users" hash table points to this kind of data:
 */

typedef struct {
    int flags;
    char pwd[32];
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
static Tcl_ObjCmdProc PermObjCmd;
static Tcl_ObjCmdProc AddUserObjCmd;
static Tcl_ObjCmdProc DelUserObjCmd;
static Tcl_ObjCmdProc AddGroupObjCmd;
static Tcl_ObjCmdProc DelGroupObjCmd;
static Tcl_ObjCmdProc ListUsersObjCmd;
static Tcl_ObjCmdProc ListGroupsObjCmd;
static Tcl_ObjCmdProc ListPermsObjCmd;
static Tcl_ObjCmdProc DelPermObjCmd;
static Tcl_ObjCmdProc CheckPassObjCmd;
static Tcl_ObjCmdProc SetPassObjCmd;

static int AllowDenyObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv, int allow, int user);

static int ValidateUserAddr(User * userPtr, const char *peer);
static int AuthProc(const char *server, const char *method, const char *url, 
		    const char *user, const char *pwd, const char *peer);
static void WalkCallback(Tcl_DString * dsPtr, void *arg);
static int CreateNonce(const char *privatekey, char **nonce, char *uri);
static int CreateHeader(Server * servPtr, Ns_Conn * conn, int stale);
/*static int CheckNonce(const char *privatekey, char *nonce, char *uri, int timeout);*/

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
 *	Initialize the perms module
 *
 * Results:
 *	NS_OK/NS_ERROR
 *
 * Side effects:
 *	Init hash table, add tcl commands.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT int
Ns_ModuleInit(char *server, char *module)
{
    Server *servPtr;
    /*char *path;*/
    Tcl_HashEntry *hPtr;
    int isNew, result;

    if (uskey < 0) {
        double d;
        char buf[TCL_INTEGER_SPACE];
        Ns_CtxMD5 md5;
        unsigned long result;
        unsigned char sig[16];

        uskey = Ns_UrlSpecificAlloc();
        Tcl_InitHashTable(&serversTable, TCL_STRING_KEYS);

        /* Make a really big random number */
        d = Ns_DRand();
        result = (unsigned long) (d * 1024 * 1024 * 1024);

        /* There is no requirement to hash it but it won't hurt */
        Ns_CtxMD5Init(&md5);
        snprintf(buf, sizeof(buf), "%lu", result);
        Ns_CtxMD5Update(&md5, (unsigned char *) buf, strlen(buf));
        Ns_CtxMD5Final(&md5, sig);
        Ns_CtxString(sig, usdigest, 16);
    }
    servPtr = ns_malloc(sizeof(Server));
    servPtr->server = server;
    /*path = Ns_ConfigGetPath(server, module, NULL);*/
    Tcl_InitHashTable(&servPtr->users, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->groups, TCL_STRING_KEYS);
    Ns_RWLockInit(&servPtr->lock);
    Ns_SetRequestAuthorizeProc(server, AuthProc);
    result = Ns_TclRegisterTrace(server, AddCmds, servPtr, NS_TCL_TRACE_CREATE);
    hPtr = Tcl_CreateHashEntry(&serversTable, server, &isNew);
    Tcl_SetHashValue(hPtr, servPtr);
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AddCmds --
 *
 *	Add tcl commands for perms
 *
 * Results:
 *	NS_OK
 *
 * Side effects:
 *	Adds tcl commands
 *
 *----------------------------------------------------------------------
 */

static int AddCmds(Tcl_Interp * interpermPtr, void *arg)
{
    Tcl_CreateObjCommand(interpermPtr, "ns_perm", PermObjCmd, arg, NULL);
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PermCmd --
 *
 *	The ns_perm tcl command
 *
 * Results:
 *	Std tcl ret val
 *
 * Side effects:
 *	Yes.
 *
 *----------------------------------------------------------------------
 */

static int PermObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    int opt, status = TCL_OK;

    static CONST char *opts[] = {
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
        cmdDelUser, cmdDelGroup, cmdDelPerm,
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?args ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case cmdAddUser:
        status = AddUserObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdDelUser:
        status = DelUserObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdAddGroup:
        status = AddGroupObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdDelGroup:
        status = DelGroupObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdListUsers:
        status = ListUsersObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdListGroups:
        status = ListGroupsObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdListPerms:
        status = ListPermsObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdDelPerm:
        status = DelPermObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdAllowUser:
        status = AllowDenyObjCmd(servPtr, interp, objc, objv, 1, 1);
        break;

    case cmdDenyUser:
        status = AllowDenyObjCmd(servPtr, interp, objc, objv, 0, 1);
        break;

    case cmdAllowGroup:
        status = AllowDenyObjCmd(servPtr, interp, objc, objv, 1, 0);
        break;

    case cmdDenyGroup:
        status = AllowDenyObjCmd(servPtr, interp, objc, objv, 0, 0);
        break;

    case cmdCheckPass:
        status = CheckPassObjCmd(servPtr, interp, objc, objv);
        break;

    case cmdSetPass:
        status = SetPassObjCmd(servPtr, interp, objc, objv);
        break;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * AuthProc --
 *
 *	Authorize a URL--this callback is called when a new
 *	connection is recieved

 *	Digest authentication per RFC 2617 but currently
 * 	supports qop="auth" and MD5 hashing only.
 *
 *	The logic goes like this:
 *	 - fetch the Authorization header
 *	   if it exists, continue
 *	   if it doesn't exist, return an Unauthorized header and
 *	   WWW-Authenticate header.
 *	 - Parse the Authorization header and perform digest authentication
 *	   against it.

 * Results:
 *	NS_OK: accept;
 *	NS_FORBIDDEN or NS_UNAUTHORIZED: go away;
 *	NS_ERROR: oops
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int AuthProc(const char *server, const char *method, const char *url, 
		    const char *user, const char *pwd, const char *peer)
{
    int status;
    Ns_Set *set;
    Server *servPtr;
    Perm *permPtr;
    User *userPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    char buf[NS_ENCRYPT_BUFSIZE], *group, *auth = NULL;
    Ns_Conn *conn = Ns_GetConn();

    if (user == NULL) {
        user = "";
    }
    if (pwd == NULL) {
        pwd = "";
    }

    hPtr = Tcl_FindHashEntry(&serversTable, server);
    if (hPtr == NULL) {
        return NS_FORBIDDEN;
    }
    servPtr = Tcl_GetHashValue(hPtr);

    Ns_RWLockRdLock(&servPtr->lock);
    permPtr = Ns_UrlSpecificGet(server, method, url, uskey);
    if (permPtr == NULL) {
        status = NS_OK;
        goto done;
    }

    /*
     * Make sure we have parsed Authentication header properly,
     * otherwise fallback to Basic method
     */

    set = Ns_ConnAuth(conn);
    if (set != NULL) {
        auth = Ns_SetIGet(set, "AuthMethod");
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

    hPtr = Tcl_FindHashEntry(&servPtr->users, user);
    if (hPtr == NULL) {
        goto done;
    }
    userPtr = Tcl_GetHashValue(hPtr);

    /*
     * Check which auth method to use, permission record will
     * define how to verify user
     */

    if (STREQ(auth, "Basic")) {

        /*
         * Basic Authentiction: Verify user password (if any).
         */

        if (userPtr->pwd[0] != 0) {
            if (pwd[0] == 0) {
                goto done;
            }
            if (!(userPtr->flags & USER_CLEAR_TEXT)) {
                Ns_Encrypt(pwd, userPtr->pwd, buf);
                pwd = buf;
            }
            if (!STREQ(userPtr->pwd, pwd)) {
                goto done;
            }
        }
    } else {

        /*
         * Digest Authentication
         */

	if (STREQ(auth, "Digest")) {

	}
    }
    
    /*
     * Check for a vaild user address.
     */

    if (!ValidateUserAddr(userPtr, peer)) {
        /*
         * Null user never gets forbidden--give a chance to enter password.
         */
      deny:
        if (*user != '\0') {
            status = NS_FORBIDDEN;
        }
        goto done;
    }

    /*
     * Check user deny list.
     */

    if (Tcl_FindHashEntry(&permPtr->denyuser, user) != NULL) {
        goto deny;
    }

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

    /*
     * Checks above failed.  If implicit allow is not set,
     * change the status back to unauthorized. This flag will be set only when
     * at least one deny user was added to the the permission record, otherwise
     * it will allow user with name "" to pass. What a nonsense!
     */

    if (!(permPtr->flags & PERM_IMPLICIT_ALLOW)) {
        status = NS_UNAUTHORIZED;
    }

  done:

    /*
     * For Digest authentication we create WWW-Authenticate header manually
     */

    if (status == NS_UNAUTHORIZED && !strcmp(auth, "Digest")) {
        int stale = NS_FALSE;
        CreateHeader(servPtr, conn, stale);
    }

    Ns_RWLockUnlock(&servPtr->lock);
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ValidateUserAddr --
 *
 *	Validate that the peer address is valid for this user
 *
 * Results:
 *	NS_TRUE if allowed, NS_FALSE if not
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int ValidateUserAddr(User *userPtr, const char *peer)
{
    struct in_addr peerip, ip, mask;
    int retval;
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;

    if (peer == NULL) {
        return NS_TRUE;
    }

    peerip.s_addr = inet_addr(peer);
    if (peerip.s_addr == INADDR_NONE) {
        return NS_FALSE;
    }

    /*
     * Loop over each netmask, AND the peer address with it,
     * then see if that address is in the list.
     */

    hPtr = Tcl_FirstHashEntry(&userPtr->masks, &search);
    while (hPtr != NULL) {
	Tcl_HashEntry *entryPtr;

        mask.s_addr = (unsigned long) Tcl_GetHashKey(&userPtr->masks, hPtr);
        ip.s_addr = peerip.s_addr & mask.s_addr;

        /*
         * There is a potential match. Now make sure it works with the
         * right address's mask.
         */

        entryPtr = Tcl_FindHashEntry(&userPtr->nets, (char *) (intptr_t) ip.s_addr);
        if (entryPtr != NULL && mask.s_addr == (unsigned long) (intptr_t) Tcl_GetHashValue(entryPtr)) {

            if (userPtr->flags & USER_FILTER_ALLOW) {
                return NS_TRUE;
            } else {
                return NS_FALSE;
            }
        }
        hPtr = Tcl_NextHashEntry(&search);
    }

    if (userPtr->flags & USER_FILTER_ALLOW) {
        retval = NS_FALSE;
    } else {
        retval = NS_TRUE;
    }
    if (userPtr->hosts.numEntries > 0) {
        Ns_DString addr;

        /*
         * If we have gotten this far, it's necessary to do a
         * reverse dns lookup and try to make a decision
         * based on that, if possible.
         */

        Ns_DStringInit(&addr);
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
                        retval = NS_TRUE;
                    } else {
                        retval = NS_FALSE;
                    }
                    break;
                }
                start = strchr(start + 1, '.');
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

    return retval;
}


#if !defined(HAVE_INET_PTON)
/*
 *----------------------------------------------------------------------
 *
 * inet_aton --
 *
 *	inet_aton for windows. 
 *
 * Results:
 *	0/1
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
# ifdef _WIN32
# include <winsock.h>

int inet_aton(const char *addrString, struct in_addr *addr) {
  addr->s_addr = inet_addr(addrString);
  return (addr->s_addr == INADDR_NONE) ? 0 : 1;
}
# endif
#endif

/*
 *----------------------------------------------------------------------
 *
 * AddUserCmd --
 *
 *	Implements the Tcl command ns_perm adduser
 *
 * Results:
 *	Tcl resut
 *
 * Side effects:
 *	A user may be added to the global user hash table
 *
 *----------------------------------------------------------------------
 */

static int AddUserObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    User *userPtr;
    Group *groupPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    struct in_addr ip, mask;
    char buf[NS_ENCRYPT_BUFSIZE];
    char *name, *slash, *net, *pwd;
    char *field = NULL, *salt = NULL;
    int isNew, i, nargs = 0, allow = 0, deny = 0, clear = 0;

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
        {"pwd", Ns_ObjvString, &pwd, NULL},
        {"field", Ns_ObjvString, &field, NULL},
        {"?hosts", Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    userPtr = ns_calloc(1U, sizeof(User));
    if (clear) {
        userPtr->flags |= USER_CLEAR_TEXT;
    }
    if (salt != NULL) {
        Ns_Encrypt(pwd, salt, buf);
        pwd = buf;
        userPtr->flags &= ~USER_CLEAR_TEXT;
    }
    snprintf(userPtr->pwd, sizeof(userPtr->pwd), "%s", pwd);
    Tcl_InitHashTable(&userPtr->nets, TCL_ONE_WORD_KEYS);
    Tcl_InitHashTable(&userPtr->masks, TCL_ONE_WORD_KEYS);
    Tcl_InitHashTable(&userPtr->hosts, TCL_STRING_KEYS);
    Tcl_InitHashTable(&userPtr->groups, TCL_STRING_KEYS);

    /*
     * Both -allow and -deny can be used for consistency, but
     * -deny has precedence
     */

    if (allow && !deny) {
        userPtr->flags |= USER_FILTER_ALLOW;
    }

    /*
     * Loop over each parameter and figure out what it is. The
     * possiblities are ipaddr/netmask, hostname, or partial hostname:
     * 192.168.2.3/255.255.255.0, foo.bar.com, or .bar.com
     */

    for (i = objc - nargs; i < objc; ++i) {
        mask.s_addr = INADDR_NONE;
        net = Tcl_GetString(objv[i]);
        slash = strchr(net, '/');
        if (slash == NULL) {
	  (void)Tcl_CreateHashEntry(&userPtr->hosts, net, &isNew);
        } else {

            /*
             * Try to conver the IP address/netmask into binary
             * values.
             */

            *slash = '\0';
#if defined(HAVE_INET_PTON)
            if (inet_pton(AF_INET, net, &ip) == 0 || inet_pton(AF_INET, slash+1, &mask) == 0) 
#else
            if (inet_aton(net, &ip) == 0 || inet_aton(slash+1, &mask) == 0) 
#endif
	    {
                Tcl_AppendResult(interp, "invalid address or hostname \"",
                                 net, "\". " "should be ipaddr/netmask or hostname", NULL);
                goto fail;
            }

            /*
             * Do a bitwise AND of the ip address with the netmask
             * to make sure that all non-network bits are 0. That
             * saves us from doing this operation every time a
             * connection comes in.
             */

            ip.s_addr &= mask.s_addr;

            /*
             * Is this a new netmask? If so, add it to the list.
             * A list of netmasks is maintained and every time a
             * new connection comes in, the peer address is ANDed with
             * each of them and a lookup on that address is done
             * on the hash table of networks.
             */

            (void) Tcl_CreateHashEntry(&userPtr->masks, (char *) (intptr_t) mask.s_addr, &isNew);

            hPtr = Tcl_CreateHashEntry(&userPtr->nets, (char *) (intptr_t) ip.s_addr, &isNew);
            Tcl_SetHashValue(hPtr, (ClientData) (intptr_t) mask.s_addr);
        }
        if (!isNew) {
            Tcl_AppendResult(interp, "duplicate entry: ", net, NULL);
            goto fail;
        }
    }

    /*
     * Add the user.
     */

    Ns_RWLockWrLock(&servPtr->lock);
    hPtr = Tcl_CreateHashEntry(&servPtr->users, name, &isNew);
    if (!isNew) {
        Tcl_AppendResult(interp, "duplicate user: ", name, NULL);
        goto fail0;
    }
    Tcl_SetHashValue(hPtr, userPtr);
    Ns_RWLockUnlock(&servPtr->lock);
    return TCL_OK;

  fail0:
    Ns_RWLockUnlock(&servPtr->lock);

  fail:
    hPtr = Tcl_FirstHashEntry(&userPtr->groups, &search);
    while (hPtr != NULL) {
        groupPtr = Tcl_GetHashValue(hPtr);
        hPtr = Tcl_FindHashEntry(&groupPtr->users, name);
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&userPtr->groups);
    Tcl_DeleteHashTable(&userPtr->masks);
    Tcl_DeleteHashTable(&userPtr->nets);
    Tcl_DeleteHashTable(&userPtr->hosts);
    ns_free(userPtr);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * DelUserCmd --
 *
 *	Implements the Tcl command ns_perm deluser
 *
 * Results:
 *	Tcl resut
 *
 * Side effects:
 *	A user may be deleted from the global user hash table
 *
 *----------------------------------------------------------------------
 */

static int DelUserObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    char *name = NULL;
    User *userPtr = NULL;
    Group *groupPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    Ns_ObjvSpec args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    Ns_RWLockWrLock(&servPtr->lock);
    hPtr = Tcl_FindHashEntry(&servPtr->users, name);
    if (hPtr != NULL) {
        userPtr = Tcl_GetHashValue(hPtr);
        Tcl_DeleteHashEntry(hPtr);
    }
    Ns_RWLockUnlock(&servPtr->lock);

    if (userPtr != NULL) {
        hPtr = Tcl_FirstHashEntry(&userPtr->groups, &search);
        while (hPtr != NULL) {
            groupPtr = Tcl_GetHashValue(hPtr);
            hPtr = Tcl_FindHashEntry(&groupPtr->users, name);
            if (hPtr != NULL) {
                Tcl_DeleteHashEntry(hPtr);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&userPtr->groups);
        Tcl_DeleteHashTable(&userPtr->masks);
        Tcl_DeleteHashTable(&userPtr->nets);
        Tcl_DeleteHashTable(&userPtr->hosts);
        ns_free(userPtr);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ListUsersCmd --
 *
 *	Implements the Tcl command ns_perm listusers
 *
 * Results:
 *	Tcl resut
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int ListUsersObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    struct in_addr ip;
    Tcl_HashSearch search, msearch;
    Tcl_HashEntry *hPtr;

    Ns_RWLockRdLock(&servPtr->lock);
    hPtr = Tcl_FirstHashEntry(&servPtr->users, &search);

    while (hPtr != NULL) {
	Tcl_HashEntry *mPtr;
	User          *userPtr = Tcl_GetHashValue(hPtr);

        Tcl_AppendResult(interp, "{", Tcl_GetHashKey(&servPtr->users, hPtr), "} {", userPtr->pwd, "} {", NULL);

        if (userPtr->hosts.numEntries > 0 || userPtr->masks.numEntries > 0 || userPtr->nets.numEntries > 0) {
            Tcl_AppendResult(interp, userPtr->flags & USER_FILTER_ALLOW ? " -allow " : " -deny ", NULL);
        }
        mPtr = Tcl_FirstHashEntry(&userPtr->nets, &msearch);
        while (mPtr != NULL) {
            ip.s_addr = (unsigned long) Tcl_GetHashKey(&userPtr->nets, mPtr);
            Tcl_AppendResult(interp, ns_inet_ntoa(ip), " ", NULL);
            mPtr = Tcl_NextHashEntry(&msearch);
        }

        mPtr = Tcl_FirstHashEntry(&userPtr->masks, &msearch);
        while (mPtr != NULL) {
            ip.s_addr = (unsigned long) Tcl_GetHashKey(&userPtr->masks, mPtr);
            Tcl_AppendResult(interp, ns_inet_ntoa(ip), " ", NULL);
            mPtr = Tcl_NextHashEntry(&msearch);
        }

        mPtr = Tcl_FirstHashEntry(&userPtr->hosts, &msearch);
        while (mPtr != NULL) {
            Tcl_AppendResult(interp, Tcl_GetHashKey(&userPtr->hosts, mPtr), " ", NULL);
            mPtr = Tcl_NextHashEntry(&msearch);
        }
        Tcl_AppendResult(interp, "} ", NULL);

        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_RWLockUnlock(&servPtr->lock);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * AddGroupCmd --
 *
 *	Add a group to the global groups list
 *
 * Results:
 *	Standard tcl
 *
 * Side effects:
 *	A group will be created
 *
 *----------------------------------------------------------------------
 */

static int AddGroupObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    char *name, *user;
    User *userPtr;
    Group *groupPtr;
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;
    int isNew, param;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "name user ?user ...?");
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
     * it's ok, and add him. Also put the group into the user's list
     * of groups he's in.
     */

    for (param = 3; param < objc; param++) {
        user = Tcl_GetString(objv[param]);
        hPtr = Tcl_FindHashEntry(&servPtr->users, user);
        if (hPtr == NULL) {
            Tcl_AppendResult(interp, "no such user: ", user, NULL);
            goto fail;
        }
        userPtr = Tcl_GetHashValue(hPtr);

        /*
         * Add the user to the group's list of users
         */

        hPtr = Tcl_CreateHashEntry(&groupPtr->users, user, &isNew);
        if (!isNew) {
          dupuser:
            Tcl_AppendResult(interp, "user \"", user, "\" already in group \"", name, "\"", NULL);
            goto fail;
        }
        Tcl_SetHashValue(hPtr, userPtr);

        /*
         * Add the group to the user's list of groups
         */

        hPtr = Tcl_CreateHashEntry(&userPtr->groups, name, &isNew);
        if (!isNew) {
            goto dupuser;
        }
        Tcl_SetHashValue(hPtr, groupPtr);
    }

    /*
     * Add the group to the global list of groups
     */

    Ns_RWLockWrLock(&servPtr->lock);
    hPtr = Tcl_CreateHashEntry(&servPtr->groups, name, &isNew);
    if (!isNew) {
        Tcl_AppendResult(interp, "duplicate group: ", name, NULL);
        goto fail0;
    }
    Tcl_SetHashValue(hPtr, groupPtr);
    Ns_RWLockUnlock(&servPtr->lock);
    return TCL_OK;

  fail0:
    Ns_RWLockUnlock(&servPtr->lock);

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
 * DelGroupCmd --
 *
 *	Implements the Tcl command ns_perm delgroup
 *
 * Results:
 *	Tcl resut
 *
 * Side effects:
 *	A group may be deleted from the global user hash table
 *
 *----------------------------------------------------------------------
 */

static int DelGroupObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
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

    Ns_RWLockWrLock(&servPtr->lock);
    hPtr = Tcl_FindHashEntry(&servPtr->groups, name);
    if (hPtr) {
        groupPtr = Tcl_GetHashValue(hPtr);
        Tcl_DeleteHashEntry(hPtr);
    }
    Ns_RWLockUnlock(&servPtr->lock);

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
 * ListGroupsCmd --
 *
 *	Implements the Tcl command ns_perm listgroups
 *
 * Results:
 *	Tcl resut
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int ListGroupsObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;

    Ns_RWLockRdLock(&servPtr->lock);
    hPtr = Tcl_FirstHashEntry(&servPtr->groups, &search);
    while (hPtr != NULL) {
	Tcl_HashSearch usearch;
	Tcl_HashEntry *uhPtr;
	Group         *groupPtr = Tcl_GetHashValue(hPtr);

        Tcl_AppendResult(interp, Tcl_GetHashKey(&servPtr->groups, hPtr), " { ", NULL);

        /*
         * All users for this group
         */

        uhPtr = Tcl_FirstHashEntry(&groupPtr->users, &usearch);
        while (uhPtr != NULL) {
            Tcl_AppendResult(interp, "\"", Tcl_GetHashKey(&groupPtr->users, uhPtr), "\" ", NULL);
            uhPtr = Tcl_NextHashEntry(&usearch);
        }
        Tcl_AppendResult(interp, "} ", NULL);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_RWLockUnlock(&servPtr->lock);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * AllowDenyObjCmd --
 *
 *	Add a record that will allow or deny access to the specified url
 *
 * Results:
 *	Std tcl
 *
 * Side effects:
 *	A perm record may be created
 *
 *----------------------------------------------------------------------
 */

static int AllowDenyObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv, int allow, int user)
{
    Server      *servPtr = data;
    Perm        *permPtr;
    Ns_DString   base;
    char        *method, *url;
    int          i, isNew, noinherit = 0, nargs = 0;
    unsigned int flags = 0U;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,   &flags,  INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method", Ns_ObjvString, &method, NULL},
        {"url", Ns_ObjvString, &url, NULL},
        {"users", Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit) {flags |= NS_OP_NOINHERIT;}

    /*
     * Construct the base url.
     */

    Ns_DStringInit(&base);
    Ns_NormalizePath(&base, url);

    /*
     * Locate and verify the exact record.
     */

    Ns_RWLockWrLock(&servPtr->lock);
    permPtr = Ns_UrlSpecificGet(servPtr->server, method, url, uskey);

    if (permPtr != NULL && !STREQ(base.string, permPtr->baseurl)) {
        permPtr = NULL;
    }
    if (permPtr == NULL) {
        permPtr = ns_calloc(1U, sizeof(Perm));
        permPtr->baseurl = Ns_DStringExport(&base);
        Tcl_InitHashTable(&permPtr->allowuser, TCL_STRING_KEYS);
        Tcl_InitHashTable(&permPtr->denyuser, TCL_STRING_KEYS);
        Tcl_InitHashTable(&permPtr->allowgroup, TCL_STRING_KEYS);
        Tcl_InitHashTable(&permPtr->denygroup, TCL_STRING_KEYS);
        Ns_UrlSpecificSet(servPtr->server, method, url, uskey, permPtr, flags, NULL);
    }
    if (!allow) {
        permPtr->flags |= PERM_IMPLICIT_ALLOW;
    }

    for (i = objc - nargs; i < objc; i++) {
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
    Ns_RWLockUnlock(&servPtr->lock);
    Ns_DStringFree(&base);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DelPermObjCmd --
 *
 *	Remove permission record
 *
 * Results:
 *	Std tcl
 *
 * Side effects:
 *	A perm record may be deleted
 *
 *----------------------------------------------------------------------
 */

static int DelPermObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server      *servPtr = data;
    Perm        *permPtr;
    Ns_DString   base;
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
    if (noinherit) {flags |= NS_OP_NOINHERIT;}

    /*
     * Construct the base url.
     */

    Ns_DStringInit(&base);
    Ns_NormalizePath(&base, url);

    /*
     * Locate and verify the exact record.
     */

    Ns_RWLockWrLock(&servPtr->lock);
    permPtr = Ns_UrlSpecificGet(servPtr->server, method, url, uskey);
    if (permPtr != NULL) {
        Ns_UrlSpecificDestroy(servPtr->server, method, url, uskey, flags);
        ns_free(permPtr->baseurl);
        Tcl_DeleteHashTable(&permPtr->allowuser);
        Tcl_DeleteHashTable(&permPtr->denyuser);
        Tcl_DeleteHashTable(&permPtr->allowgroup);
        Tcl_DeleteHashTable(&permPtr->denygroup);
        ns_free(permPtr);
    }
    Ns_RWLockUnlock(&servPtr->lock);
    Ns_DStringFree(&base);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ListPermsCmd --
 *
 *	Implements the Tcl command ns_perm listperms
 *
 * Results:
 *	Tcl resut
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int ListPermsObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    Ns_DString ds;

    Ns_DStringInit(&ds);
    Ns_RWLockRdLock(&servPtr->lock);
    Ns_UrlSpecificWalk(uskey, servPtr->server, WalkCallback, &ds);
    Ns_RWLockUnlock(&servPtr->lock);
    Tcl_AppendResult(interp, ds.string, NULL);
    Ns_DStringFree(&ds);
    return TCL_OK;
}

static void WalkCallback(Tcl_DString * dsPtr, void *arg)
{
    Perm *permPtr = arg;
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;

    if (permPtr->flags & PERM_IMPLICIT_ALLOW) {
        Ns_DStringAppend(dsPtr, " -implicitallow ");
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->allowuser, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -allowuser {", Tcl_GetHashKey(&permPtr->allowuser, hPtr), "}", NULL);
        hPtr = Tcl_NextHashEntry(&search);
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->denyuser, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -denyuser {", Tcl_GetHashKey(&permPtr->denyuser, hPtr), "}", NULL);
        hPtr = Tcl_NextHashEntry(&search);
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->allowgroup, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -allowgroup {", Tcl_GetHashKey(&permPtr->allowgroup, hPtr), "}", NULL);
        hPtr = Tcl_NextHashEntry(&search);
    }

    hPtr = Tcl_FirstHashEntry(&permPtr->denygroup, &search);
    while (hPtr != NULL) {
        Ns_DStringVarAppend(dsPtr, " -denygroup {", Tcl_GetHashKey(&permPtr->denygroup, hPtr), "}", NULL);
        hPtr = Tcl_NextHashEntry(&search);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CheckPassCmd --
 *
 *	Checks supplied user password against internak database
 *
 * Results:
 *	1 if verified, 0 if not valid
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int CheckPassObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    int rc = TCL_ERROR;
    User *userPtr;
    char *user, *pwd;
    Tcl_HashEntry *hPtr;

    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "user pwd");
        return TCL_ERROR;
    }
    user = Tcl_GetString(objv[2]);
    pwd = Tcl_GetString(objv[3]);

    Ns_RWLockRdLock(&servPtr->lock);
    hPtr = Tcl_FindHashEntry(&servPtr->users, user);
    if (hPtr == NULL) {
        Tcl_AppendResult(interp, "user not found", NULL);
        goto done;
    }
    userPtr = Tcl_GetHashValue(hPtr);
    if (userPtr->pwd[0] != 0) {
        char buf[NS_ENCRYPT_BUFSIZE];

        if (pwd[0] == 0) {
            Tcl_AppendResult(interp, "empty password given", NULL);
            goto done;
        }
        Ns_Encrypt(pwd, userPtr->pwd, buf);
        if (!STREQ(userPtr->pwd, buf)) {
            Tcl_AppendResult(interp, "incorrect password", NULL);
            goto done;
        }
    }
    rc = TCL_OK;

  done:
    Ns_RWLockUnlock(&servPtr->lock);
    return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * SetPassCmd --
 *
 *	Assigns new password to the user
 *
 * Results:
 *	1 if assigned, 0 if not found
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int SetPassObjCmd(ClientData data, Tcl_Interp * interp, int objc, Tcl_Obj *CONST* objv)
{
    Server *servPtr = data;
    int rc = 0;
    User *userPtr;
    Tcl_HashEntry *hPtr;
    char *user, *pwd, *salt = NULL;
    char buf[NS_ENCRYPT_BUFSIZE];

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "user pwd ?salt?");
        return TCL_ERROR;
    }
    user = Tcl_GetString(objv[2]);
    pwd = Tcl_GetString(objv[3]);
    salt = objc > 4 ? Tcl_GetString(objv[4]) : NULL;

    Ns_RWLockRdLock(&servPtr->lock);
    hPtr = Tcl_FindHashEntry(&servPtr->users, user);
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
    Ns_RWLockUnlock(&servPtr->lock);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateNonce --
 *
 *	Create the nonce to be used by the client to hash against.
 *	The hash is a uuencoded string that consists of:
 *
 *	time-stamp H(time-stamp ":" uri ":" private-key)
 *
 *	Note that this function is called here with uri = ""
 *
 * Results:
 *	NS_OK/NS_ERROR
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
*/

static int CreateNonce(const char *privatekey, char **nonce, char *uri)
{
    time_t now;
    Ns_DString ds;
    Ns_CtxMD5 md5;
    unsigned char sig[16];
    char buf[33];
    char bufcoded[1 + (4 * 48) / 2];

    if (!privatekey) {
        return NS_ERROR;
    }

    now = time(NULL);

    Ns_DStringInit(&ds);
    Ns_DStringPrintf(&ds, "%" PRIu64 ":%s:%s", (int64_t) now, uri, privatekey);

    Ns_CtxMD5Init(&md5);
    Ns_CtxMD5Update(&md5, (unsigned char *) ds.string, (unsigned int) ds.length);
    Ns_CtxMD5Final(&md5, sig);
    Ns_CtxString(sig, buf, 16);

    /* encode the current time and MD5 string into the nonce */
    Ns_DStringTrunc(&ds, 0);
    Ns_DStringPrintf(&ds, "%" PRIu64 " %s", (int64_t) now, buf);
    Ns_HtuuEncode((unsigned char *) ds.string, (unsigned int) ds.length, bufcoded);

    *nonce = ns_strdup(bufcoded);

    return NS_OK;
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
 *	Given a nonce value ensure that it hasn't been tampered with
 *	and that it isn't stale. The hash is a uuencoded string that
 *	consists of:
 *
 *	time-stamp H(time-stamp ":" uri ":" private-key)
 *
 * Results:
 *	NS_OK or NS_ERROR if the nonce is stale.
 *
 * Side effects:
 *	None.
 */

static int CheckNonce(const char *privatekey, char *nonce, char *uri, int timeout)
{
    Ns_CtxMD5 md5;
    Ns_DString ds;
    char buf[33];
    char *decoded;
    char *ntime;
    char *tnonce;
    int n, rv = NS_OK;
    unsigned char sig[16];
    time_t now, nonce_time;

    if (!privatekey) {
        return NS_ERROR;
    }

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

    Ns_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, ntime, ":", uri, ":", privatekey, NULL);

    Ns_CtxMD5Update(&md5, (unsigned char *) ds.string, (unsigned int) ds.length);
    Ns_CtxMD5Final(&md5, sig);
    Ns_CtxString(sig, buf, 16);

    /* Check for a stale time stamp. If the time stamp is stale we still check
     * to see if the user sent the proper digest password. The stale flag
     * is only set if the nonce is expired AND the credentials are OK, otherwise
     * the get a 401, but that happens elsewhere.
     */

    nonce_time = (time_t) strtol(ntime, (char **) NULL, 10);

    if ((now - nonce_time) > timeout) {
        rv = NS_ERROR;
    }

    if (!(STREQ(tnonce, buf))) {
        rv = NS_ERROR;
    }

    return rv;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * CreateHeader --
 *
 *	Assigns WWW-Authenticate headers according to Digest
 *	authentication rules.
 *
 * Results:
 *	NS_OK/NS_ERROR
 *
 * Side effects:
 *	Appends HTTP headers to output headers
 *
 *----------------------------------------------------------------------
*/

static int CreateHeader(Server * servPtr, Ns_Conn * conn, int stale)
{
    Ns_DString ds;
    char *nonce = 0;

    if (CreateNonce(usdigest, &nonce, "") == NS_ERROR) {
        return NS_ERROR;
    }

    Ns_DStringInit(&ds);
    Ns_DStringPrintf(&ds, "Digest realm=\"%s\", nonce=\"%s\", algorithm=\"MD5\", qop=\"auth\"", servPtr->server, nonce);

    if (stale == NS_TRUE) {
        Ns_DStringVarAppend(&ds, ", stale=\"true\"", NULL);
    }
    Ns_ConnSetHeaders(conn, "WWW-Authenticate", ds.string);
    return NS_OK;
}
