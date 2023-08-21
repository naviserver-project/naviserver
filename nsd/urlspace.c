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
 * urlspace.c --
 *
 *      This file implements a Trie data structure. It is used
 *      for "UrlSpecificData"; for example, when one registers
 *      a handler for all GET /foo/bar/ *.html requests, the data
 *      structure that holds that information is implemented herein.
 *
 */

/*
 * There are four basic data structures used in maintaining the urlspace
 * trie. They are:
 *
 * 1. Junction
 *    A junction is nothing more than a list of channels.
 * 2. Channel
 *    A channel points to a branch which ultimately leads to nodes
 *    that match a particular "filter", such as "*.html". The filter
 *    is the last section of a URL mask, and is the only part of
 *    the mask that may contain wildcards.
 * 3. Branch
 *    A branch represents one part of a URL, such as a method or directory
 *    component. It has a list of branches representing sub-URLs and a
 *    pointer to a Node, if data was registered for this specific branch.
 * 4. Node
 *    A node stores URL-specific data, as well as a pointer to the
 *    cleanup function.
 *
 * Another data structure, called an Index, which is manipulated by the
 * Ns_Index API calls, is used by the urlspace code. An Index is an
 * ordered list of pointers. The order is determined by callback
 * functions. See index.c for the scoop on that.
 *
 * Here is what the urlspace data structure would look like after
 * calling:
 *
 *
 * myId = Ns_UrlSpecificAlloc();
 *
 * Ns_UrlSpecificSet("server1", "GET", "/foo/bar/\*.html", myID, myData,
 *                   0u, MyDeleteProc);
 *
 *
 *
 *  NsServer->urlspace.junction[]: Junction[] [*][ ][ ][ ][ ]
 *                                             |
 *    +----------------------------------------+
 *    |
 *    V
 *  Junction
 *    byname: Ns_Index [*][ ][ ][ ][ ]
 *                      |
 *    +-----------------+
 *    |
 *    V
 *  Channel
 *    filter: char* "*.html"
 *    trie:   Trie
 *              node:      Node*     (NULL)
 *              branches:  Ns_Index  [*][ ][ ][ ][ ]
 *                                    |
 *    +-------------------------------+
 *    |
 *    V
 *  Branch
 *    word: char* "GET"
 *    trie: Trie
 *            node:      Node*       (NULL)
 *            branches:  Ns_Index    [*][ ][ ][ ][ ]
 *                                    |
 *    +-------------------------------+
 *    |
 *    V
 *  Branch
 *    word: char* "foo"
 *    trie: Trie
 *            node:      Node*       (NULL)
 *            branches:  Ns_Index    [*][ ][ ][ ][ ]
 *                                    |
 *    +-------------------------------+
 *    |
 *    V
 *  Branch
 *    word: char* "bar"
 *    trie: Trie
 *            node:      Node*       -----------------+
 *            branches:  Ns_Index    [ ][ ][ ][ ][ ]  |
 *                                                    |
 *    +-----------------------------------------------+
 *    |
 *    V
 *  Node
 *    dataInherit:         void*             myData
 *    dataNoInherit:       void*             (NULL)
 *    deletefuncInherit:   void (*)(void*)   MyDeleteProc
 *    deletefuncNoInherit: void (*)(void*)   (NULL)
 *
 */

#include "nsd.h"

#define STACK_SIZE      512 /* Max depth of URL hierarchy. */

/*
#define DEBUG 1
*/
#define CONTEXT_FILTER 1
//#define DEBUG 1

/*
 * This optimization, when turned on, prevents the server from doing a
 * whole lot of calls to Tcl_StringMatch on every lookup in urlspace.
 * Instead, an NS_strcmp is done.
 *
 * GN 2015/11: This optimization was developed more than 10 years
 * ago. With the introduction of ns_urlspace it became easy to write
 * test cases. The __URLSPACE_OPTIMIZE__ option can be turned on,
 * since it passes all tests.
 */

/*
#define __URLSPACE_OPTIMIZE__
 */

/*
 * There is still room for improvements. A simple lookup for
 * "/a/c/a.html" takes 10 strlen operations and 14 strcmp
 * operations. One could alter the static function MkSeq() to produce
 * a more intelligent structure, to calculate strlen operations once,
 * and to make it easier to access the last element.
 *
 * Currently, the performance of "ns_urlspace get" is about twice the
 * time of "nsv_get".

     ns_urlspace unset -recurse /x
     ns_urlspace set /x 1
     lappend _ [time {ns_urlspace get /x} 1000]
     lappend _ [time {ns_urlspace get /x/y} 1000]
     lappend _ [time {ns_urlspace get /x/y/z} 1000]
     nsv_set a b 1
     lappend _ [time {nsv_get a b} 1000]

 * ns_urlspace -> 0.69-0.72, nsv_get 0.39
 */


#ifdef DEBUG
static int NS_Tcl_StringMatch(const char *a, const char *b) {
    int r = Tcl_StringMatch(a, b);
    fprintf(stderr, "__TclStringMatch '%s' '%s' => %d\n", a, b, r);
    return r;
}
static size_t NS_strlen(const char *a) {
    size_t r = strlen(a);
    fprintf(stderr, "NS_strlen '%s' => %" PRIuz "\n", a, r);
    return r;
}
static int NS_strcmp(const char *a, const char *b) {
    int r = strcmp(a, b);
    fprintf(stderr, "NS_strcmp '%s' '%s' => %d\n", a, b, r);
    return r;
}
#else
#define NS_Tcl_StringMatch Tcl_StringMatch
#define NS_strlen strlen
#define NS_strcmp strcmp
#endif


/*
 * This structure defines a Node. It is the lowest-level structure in
 * urlspace and contains the data the user puts in. It holds data
 * whose scope is a set of URLs, such as /foo/bar/ *.html.
 * Data/cleanup functions are kept separately for inheriting and non-
 * inheriting URLs, as there could be overlap.
 */

typedef struct {
    void  *dataInherit;                /* User's data */
    void  *dataNoInherit;              /* User's data */
    Ns_FreeProc *deletefuncInherit;    /* Cleanup function */
    Ns_FreeProc *deletefuncNoInherit;  /* Cleanup function */
    Ns_Index data;                     /* Context filters*/
} Node;

/*
 * This structure defines a trie. A trie is a tree whose nodes are
 * branches and channels. It is an inherently recursive data
 * structure, and each node is itself a trie. Each node represents one
 * "part" of a URL; in this case, a "part" is server name, method,
 * directory, or wildcard.
 */

typedef struct {
    Ns_Index   branches;
    Node      *node;
} Trie;

/*
 * A branch is a typical node in a Trie. The "word" is the part of the
 * URL that the branch represents, and "node" is the sub-trie.
 */

typedef struct {
    char  *word;
    Trie   trie;
} Branch;

/*
 * A channel is much like a branch. It exists only at the second level
 * (Channels come out of Junctions, which are top-level structures).
 * The filter is a copy of the very last part of the URLs matched by
 * branches coming out of this channel (only branches come out of
 * channels).  When looking for a URL, the filename part of the target
 * URL is compared with the filter in each channel, and the channel is
 * traversed only if there is a match.
 */

typedef struct {
    char  *filter;
    Trie   trie;
} Channel;

/*
 * A Junction is the top-level structure. Channels come out of a junction.
 * There is one junction for each urlspecific ID.
 */

typedef struct Junction {
    Ns_Index byname;
    /*
     * We've experimented with getting rid of this index because
     * it is like byname but in semi-reverse lexicographical
     * order.  This optimization seems to work in all cases, but
     * we need a thorough way of testing all cases.
     */
#ifndef __URLSPACE_OPTIMIZE__
    Ns_Index byuse;
#endif
} Junction;

/*
 * UrlSpaceContextSpec must share fields of Ns_IndexContextSpec
 */
typedef struct UrlSpaceContextSpec {
    Ns_FreeProc  *freeProc;
    void         *data;
    Ns_FreeProc  *dataFreeProc;
    /*
     * Fields below are private.
     */
    const char   *field;
    const char   *patternString;
    struct NS_SOCKADDR_STORAGE ip;
    struct NS_SOCKADDR_STORAGE mask;
    unsigned int  specificity;
    unsigned char type;
    bool          hasPattern;
} UrlSpaceContextSpec;

/*
 * Local functions defined in this file
 */

static void  NodeDestroy(Node *nodePtr)
    NS_GNUC_NONNULL(1);

static void ContextFilterDestroy(const Ns_Index* indexPtr)
    NS_GNUC_NONNULL(1);

static void  BranchDestroy(Branch *branchPtr)
    NS_GNUC_NONNULL(1);

static Ns_IndexCmpProc CmpBranches;
static Ns_IndexCmpProc CmpUrlSpaceContextSpecs;
static Ns_IndexCmpProc CmpChannelsAsStrings;
static Ns_IndexKeyCmpProc CmpKeyWithBranch;
static Ns_IndexKeyCmpProc CmpKeyWithUrlSpaceContextSpecs;
static Ns_IndexKeyCmpProc CmpKeyWithChannelAsStrings;

#ifndef __URLSPACE_OPTIMIZE__
static Ns_IndexCmpProc CmpChannels;
static Ns_IndexKeyCmpProc CmpKeyWithChannel;
#endif

/*
 * SubCommands
 */

static Tcl_ObjCmdProc UrlSpaceGetObjCmd;
static Tcl_ObjCmdProc UrlSpaceListObjCmd;
static Tcl_ObjCmdProc UrlSpaceNewObjCmd;
static Tcl_ObjCmdProc UrlSpaceSetObjCmd;
static Tcl_ObjCmdProc UrlSpaceUnsetObjCmd;

/*
 * Utility functions
 */

static void MkSeq(Ns_DString *dsPtr, const char *method, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void WalkTrie(const Trie *triePtr, Ns_ArgProc func,
                     Ns_DString *dsPtr, char **stack, const char *filter)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static size_t CountNonWildcharChars(const char *chars)
    NS_GNUC_NONNULL(1) NS_GNUC_CONST;

#ifdef DEBUG
static void PrintSeq(const char *seq);
#endif

/*
 * Trie functions
 */

static void  TrieInit(Trie *triePtr)
    NS_GNUC_NONNULL(1);

static void  TrieAdd(Trie *triePtr, char *seq, void *data, unsigned int flags,
                     Ns_FreeProc deleteProc, void *contextSpec)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void *TrieFind(const Trie *triePtr, char *seq,
                      NsUrlSpaceContextFilterProc proc, void *context,
                      int *depthPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

static void *TrieFindExact(const Trie *triePtr, char *seq, unsigned int flags, Node **nodePtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static void *TrieDelete(const Trie *triePtr, char *seq, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void  TrieTrunc(Trie *triePtr)
    NS_GNUC_NONNULL(1);

static int   TrieTruncBranch(Trie *triePtr, char *seq)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void  TrieDestroy(Trie *triePtr)
    NS_GNUC_NONNULL(1);

/*
 * Channel functions
 */


/*
 * Junction functions
 */

static Junction *JunctionGet(NsServer *servPtr, int id)
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

static void JunctionAdd(Junction *juncPtr, char *seq, void *data,
                        unsigned int flags, Ns_FreeProc freeProc,
                        void *contextSpec)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void *JunctionFind(const Junction *juncPtr, char *seq,
                          NsUrlSpaceContextFilterProc proc, void *context)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void *JunctionFindExact(const Junction *juncPtr, char *seq, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void *JunctionDeleteNode(const Junction *juncPtr, char *seq, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void JunctionTruncBranch(const Junction *juncPtr, char *seq)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * Functions for ns_urlspace
 */
static int CheckTclUrlSpaceId(Tcl_Interp *interp, NsServer *servPtr, int *idPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int AllocTclUrlSpaceId(Tcl_Interp *interp,  int *idPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ArgProc WalkCallback;
static Ns_FreeProc UrlSpaceContextSpecFree;

/*
 * Static variables defined in this file
 */

static int nextid = 0, defaultTclUrlSpaceId = -1;
static bool tclUrlSpaces[MAX_URLSPACES] = {NS_FALSE};
static Ns_ObjvValueRange idRange = {-1, MAX_URLSPACES};


/*
 *----------------------------------------------------------------------
 *
 * CountNonWildcharChars --
 *
 *      Helper function to count non-wildcard characters to determine
 *      the specificity of a key.
 *
 * Results:
 *      Number of chars different to '*'
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static size_t
CountNonWildcharChars(const char *chars)
{
    size_t count = 0u;

    while (*chars != '\0') {
        if (*chars != '*') {
            count ++;
        }
        chars ++;
    }
    return count;
}

/*
 *----------------------------------------------------------------------
 *
 * UrlSpaceContextSpecFree, NsUrlSpaceContextSpecNew  --
 *
 *      Lifecycle function for URL context specs.
 *
 * Results:
 *      void, or fresh context spec
 *
 * Side effects:
 *      Allocating/freeing memory
 *
 *----------------------------------------------------------------------
 */

static void
UrlSpaceContextSpecFree(void *arg)
{
    UrlSpaceContextSpec *spec = arg;

    if (spec->field != NULL) {
        ns_free((void*)spec->field);
    }
    if (spec->patternString != NULL) {
        ns_free((void*)spec->patternString);
    }
    ns_free(arg);
}

NsUrlSpaceContextSpec *
NsUrlSpaceContextSpecNew(const char *field, const char *patternString)
{
    UrlSpaceContextSpec *spec;
    size_t               fieldLength;

    NS_NONNULL_ASSERT(field != NULL);
    NS_NONNULL_ASSERT(patternString != NULL);

    spec = ns_calloc(1u, sizeof(UrlSpaceContextSpec));
    spec->freeProc = UrlSpaceContextSpecFree;

    /*
     * Check, if we got something like {X-NS-ip 137.208.1.0/16}
     */
    fieldLength = strlen(field);
    //fprintf(stderr, "NsUrlSpaceContextSpecNew: headerField <%s> len %lu\n",
    //        field, fieldLength);

    if (fieldLength == 7 && strncmp(field, "X-NS-ip", 7u) == 0) {
        struct sockaddr *ipPtr   = (struct sockaddr *)&spec->ip,
                        *maskPtr = (struct sockaddr *)&spec->mask;
        Ns_ReturnCode    status;

        status = Ns_SockaddrParseIPMask(NULL, patternString, ipPtr, maskPtr, &spec->specificity);
        if (status == NS_OK) {
            //char ipString[NS_IPADDR_SIZE];
            //fprintf(stderr, "NsUrlSpaceContextSpecNew: masked IP %s\n",
            //        ns_inet_ntop(ipPtr, ipString, sizeof(ipString)));
            //fprintf(stderr, "NsUrlSpaceContextSpecNew: mask      %s\n",
            //        ns_inet_ntop(maskPtr, ipString, sizeof(ipString)));
            spec->hasPattern = (strchr(patternString, INTCHAR('/')) != NULL);
            if (maskPtr->sa_family == AF_INET) {
                spec->type = '4';
            } else {
                spec->type = '6';
            }

        } else {
            /*
             * Treat spec like header fields
             */
            spec->hasPattern = (strchr(patternString, INTCHAR('*')) != NULL);
            spec->specificity = (unsigned int)CountNonWildcharChars(patternString);
            spec->type = 'h';
        }
    } else {
        spec->hasPattern = (strchr(patternString, INTCHAR('*')) != NULL);
        spec->specificity = (unsigned int)CountNonWildcharChars(patternString);
        spec->type = 'h';
    }

    spec->field = ns_strdup(field);
    spec->patternString = ns_strdup(patternString);

    //fprintf(stderr, "NsUrlSpaceContextSpecNew: <%s %s> type %c specificity %u\n",
    //        spec->field, spec->patternString, spec->type, spec->specificity);

    return (NsUrlSpaceContextSpec *)spec;
}

/*
 *----------------------------------------------------------------------
 *
 * NsUrlSpaceContextSpecAppend  --
 *
 *      Append a UrlSpaceContextSpec to a Tcl_DString
 *      inside curly braces.
 *
 * Results:
 *      void.
 *
 * Side effects:
 *      Appending to Tcl_DString.
 *
 *----------------------------------------------------------------------
 */
const char*
NsUrlSpaceContextSpecAppend(Tcl_DString *dsPtr, NsUrlSpaceContextSpec *spec)
{
    UrlSpaceContextSpec *specPtr = (UrlSpaceContextSpec *)spec;

    Tcl_DStringAppend(dsPtr, " {", 2);
    Tcl_DStringAppendElement(dsPtr, specPtr->field);
    Tcl_DStringAppendElement(dsPtr, specPtr->patternString);
    Tcl_DStringAppend(dsPtr, "}", 1);
    return dsPtr->string;
}

/*
 *----------------------------------------------------------------------
 *
 * NsUrlSpaceContextFilter  --
 *
 *      Function of type "NsUrlSpaceContextFilterProc" the check,
 *      whether the provided context (per call) matches with the
 *      context specs, kept in the context filter definitions.
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
NsUrlSpaceContextFilter(void *contextSpec, void *context) {
    UrlSpaceContextSpec *spec = contextSpec;
    NsUrlSpaceContext   *ctx = context;
    const Ns_Set        *headers;
    bool                 success = NS_FALSE;

    headers = ctx->headers;
    if (headers != NULL && spec->type == 'h') {
        const char *s = Ns_SetIGet(headers, spec->field);
        if (s != NULL) {
            success = (Tcl_StringMatch(s, spec->patternString) != 0);
            Ns_Log(Ns_LogUrlspaceDebug, "UrlSpaceContextFilter match %s: '%s' + '%s' -> %d",
                   spec->field, s, spec->patternString, success);
        } else {
            Ns_Log(Ns_LogUrlspaceDebug, "UrlSpaceContextFilter no such header field '%s'",
                   spec->field);
        }
    } else if (spec->type == '4' || spec->type == '6') {
        const struct sockaddr *ipPtr = (const struct sockaddr *)&(spec->ip);
        const struct sockaddr *maskPtr = (const struct sockaddr *)&(spec->mask);

        success = Ns_SockaddrMaskedMatch(ctx->saPtr, maskPtr, ipPtr);
        Ns_Log(Ns_LogUrlspaceDebug, "UrlSpaceContextFilter <%s: %s> called with IP context -> %d",
               spec->field, spec->patternString, success);

        //char ipString[NS_IPADDR_SIZE];
        //fprintf(stderr, "NsUrlSpaceContextFilter: IP %s\n",
        //        ns_inet_ntop(ctx->saPtr, ipString, sizeof(ipString)));
        //fprintf(stderr, "NsUrlSpaceContextFilter: mask %s\n",
        //        ns_inet_ntop((const struct sockaddr *)&(spec->mask), ipString, sizeof(ipString)));
        //fprintf(stderr, "NsUrlSpaceContextFilter: ----> success %d\n",  success);

    } else {
        Ns_Log(Ns_LogUrlspaceDebug, "UrlSpaceContextFilter <%s: %s> called with unexpected type %c",
               spec->field, spec->patternString, spec->type);
    }
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificAlloc --
 *
 *      Allocate a unique ID to create a separate virtual URL-space.
 *
 * Results:
 *      An integer handle, or -1 on error.
 *
 * Side effects:
 *      nextid will be incremented; don't call after server startup.
 *
 *----------------------------------------------------------------------
 */

int
Ns_UrlSpecificAlloc(void)
{
    int        id;

    id = nextid++;
    if (id >= MAX_URLSPACES) {
        Ns_Fatal("Ns_UrlSpecificAlloc: NS_MAXURLSPACE exceeded: %d",
                 MAX_URLSPACES);
    }

    return id;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificSet --
 *
 *      Associate data with a set of URLs matching a wildcard, or
 *      that are simply sub-URLs.
 *
 *      Flags can be NS_OP_NOINHERIT or NS_OP_NODELETE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will set data in a urlspace trie.
 *
 *----------------------------------------------------------------------
 */
void
Ns_UrlSpecificSet(const char *server, const char *method, const char *url, int id,
                  void *data, unsigned int flags, Ns_FreeProc freeProc)
{
    Ns_UrlSpecificSet2(server, method, url, id, data, flags, freeProc, NULL);
}

void
Ns_UrlSpecificSet2(const char *server, const char *method, const char *url, int id,
                  void *data, unsigned int flags, Ns_FreeProc freeProc,
                  void *contextSpec)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(data != NULL);

    servPtr = NsGetServer(server);

    if (likely(servPtr != NULL)) {
        Ns_DString  ds;

        Ns_DStringInit(&ds);
        MkSeq(&ds, method, url);

#ifdef DEBUG
        PrintSeq(ds.string);
#endif

        JunctionAdd(JunctionGet(servPtr, id), ds.string, data, flags, freeProc, contextSpec);
        Ns_DStringFree(&ds);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificGet, Ns_UrlSpecificGetFast, Ns_UrlSpecificGetExact --
 *
 *      Find URL-specific data in the subspace identified by id that
 *      the passed-in URL matches.
 *
 *      Ns_UrlSpecificGetFast does not support wild cards.
 *      Ns_UrlSpecificGetExact does not perform URL inheritance.
 *
 * Results:
 *      A pointer to user data, set with Ns_UrlSpecificSet.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
Ns_UrlSpecificGet(const char *server, const char *method, const char *url, int id)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    servPtr = NsGetServer(server);
    return (likely(servPtr != NULL)) ?
        NsUrlSpecificGet(servPtr, method, url, id, 0u, NS_URLSPACE_DEFAULT, NULL, NULL)
        : NULL;
}

void *
Ns_UrlSpecificGetFast(const char *server, const char *method, const char *url, int id)
{
    NsServer *servPtr;

   /*
    * Deprecated Function. Use Ns_UrlSpecificGet()
    */
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    servPtr = NsGetServer(server);
    return likely(servPtr != NULL) ?
        NsUrlSpecificGet(servPtr, method, url, id, 0u, NS_URLSPACE_FAST, NULL, NULL)
        : NULL;
}

void *
Ns_UrlSpecificGetExact(const char *server, const char *method, const char *url,
                       int id, unsigned int flags)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    servPtr = NsGetServer(server);
    return likely(servPtr != NULL) ?
        NsUrlSpecificGet(servPtr, method, url, id, flags, NS_URLSPACE_EXACT, NULL, NULL)
        : NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * NsUrlSpecificGet --
 *
 *      Lower level function, receives NsServer instead of string base
 *      server name.  "flags" are just used, when NS_URLSPACE_EXACT is
 *      specified. In this case, the flags are passed to
 *      TrieFindExact(), which returns data only, which was set with
 *      this flag.
 *
 * Results:
 *      A pointer to user data, set with Ns_UrlSpecificSet.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
NsUrlSpecificGet(NsServer *servPtr, const char *method, const char *url, int id,
                 unsigned int flags, NsUrlSpaceOp op,
                 NsUrlSpaceContextFilterProc proc, void *context)
{
    Ns_DString      ds, *dsPtr = &ds;
    void           *data = NULL; /* Just to make compiler silent, we have a complete enumeration of switch values */
    const Junction *junction;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    junction = JunctionGet(servPtr, id);

    Ns_DStringInit(dsPtr);
    MkSeq(dsPtr, method, url);

#ifdef DEBUG
    fprintf(stderr, "NsUrlSpecificGet %s %s op %d\n", method, url, op);
    PrintSeq(dsPtr->string);
#endif

    switch (op) {

    case NS_URLSPACE_DEFAULT:
        data = JunctionFind(junction, dsPtr->string, proc, context);
        break;

    case NS_URLSPACE_EXACT:
        data = JunctionFindExact(junction, dsPtr->string, flags);
        break;

    case NS_URLSPACE_FAST:
        /*
         * Deprecated branch.
         */
        data = JunctionFind(junction, dsPtr->string, proc, context);
        break;

    }

    Ns_DStringFree(dsPtr);

    return data;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificDestroy --
 *
 *      Delete some urlspecific data.  Flags can be NS_OP_NODELETE,
 *      NS_OP_NOINHERIT, NS_OP_RECURSE, or NS_OP_ALLFILTERS.
 *
 * Results:
 *      A pointer to user data if not destroying recursively.
 *
 * Side effects:
 *      Will remove data from urlspace.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_UrlSpecificDestroy(const char *server, const char *method, const char *url,
                      int id, unsigned int flags)
{
    NsServer   *servPtr;
    void       *data = NULL;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    servPtr = NsGetServer(server);

    if (likely(servPtr != NULL)) {
        Ns_DString ds;

        Ns_DStringInit(&ds);
        MkSeq(&ds, method, url);
        if ((flags & NS_OP_RECURSE) != 0u) {
            //Ns_Log(Ns_LogUrlspaceDebug, "JunctionTruncBranch %s 0x%.6x", url, flags);
            JunctionTruncBranch(JunctionGet(servPtr, id), ds.string);
        } else {
            //Ns_Log(Ns_LogUrlspaceDebug, "JunctionDeleteNode %s 0x%.6x", url, flags);
            data = JunctionDeleteNode(JunctionGet(servPtr, id), ds.string, flags);
        }
        Ns_DStringFree(&ds);
    }

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificWalk --
 *
 *      Walk the urlspace calling ArgProc function for each node.
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
Ns_UrlSpecificWalk(int id, const char *server, Ns_ArgProc func, Tcl_DString *dsPtr)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(func != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        size_t          n, i;
        char           *stack[STACK_SIZE];
        const Channel  *channelPtr;
        const Junction *juncPtr = JunctionGet(servPtr, id);

        memset(stack, 0, sizeof(stack));

#ifndef __URLSPACE_OPTIMIZE__
        n = Ns_IndexCount(&juncPtr->byuse);
        for (i = 0u; i < n; i++) {
            channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
        n = Ns_IndexCount(&juncPtr->byname);
        for (i = n; i > 0u; i--) {
            channelPtr = Ns_IndexEl(&juncPtr->byname, i - 1u);
#endif
            WalkTrie(&channelPtr->trie, func, dsPtr, stack, channelPtr->filter);
        }
    }
}

static void
WalkTrie(const Trie *triePtr, Ns_ArgProc func,
         Ns_DString *dsPtr, char **stack, const char *filter)
{
    const Branch *branchPtr;
    const Node   *nodePtr;
    int           depth;
    size_t        i;
    Tcl_DString   subDs;

    NS_NONNULL_ASSERT(triePtr != NULL);
    NS_NONNULL_ASSERT(func != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(stack != NULL);
    NS_NONNULL_ASSERT(filter != NULL);

#ifdef DEBUG
    fprintf(stderr, "walk on trie %p filter %s with node %p branches %ld inc %ld '%s'\n",
            (void*)triePtr, filter, (void*)triePtr->node,
            triePtr->branches.n,
            triePtr->branches.inc,
            triePtr->branches.n > 0 ?  ((Branch*)Ns_IndexEl(&triePtr->branches, 0))->word : ""
            );
#endif
    for (i = 0u; i < triePtr->branches.n; i++) {
        branchPtr = Ns_IndexEl(&triePtr->branches, i);

        /*
         * Remember current stack depth
         */

        depth = 0;
        while (depth < STACK_SIZE -1 && stack[depth] != NULL) {
            depth++;
        }
        stack[depth] = branchPtr->word;
        WalkTrie(&branchPtr->trie, func, dsPtr, stack, filter);

        /*
         * Restore stack position
         */

        stack[depth] = NULL;
    }

    nodePtr = triePtr->node;
    //fprintf(stderr, "... node %p for appending data\n", (void*)nodePtr);

    if (nodePtr != NULL) {

        Tcl_DStringInit(&subDs);

        /*
         * Put stack contents into the sublist.
         * Element 0 is method, the rest is url
         */

        depth = 0;
        Tcl_DStringAppendElement(&subDs, stack[depth++]);
        Tcl_DStringAppend(&subDs, " ", 1);

#if 0
        Ns_DStringPrintf(&subDs, "%p:", (void*)nodePtr);
#endif
        if (stack[depth] == NULL) {
            Tcl_DStringAppendElement(&subDs, "/");
        } else {
            Tcl_DString   elementDs;

            Tcl_DStringInit(&elementDs);
            while (stack[depth] != NULL) {
                Ns_DStringVarAppend(&elementDs, "/", stack[depth], (char *)0L);
                depth++;
            }
            Tcl_DStringAppendElement(&subDs, elementDs.string);
            Tcl_DStringFree(&elementDs);

        }

        Tcl_DStringAppend(&subDs, " ", 1);
        Tcl_DStringAppendElement(&subDs, filter);
        Tcl_DStringAppend(&subDs, " ", 1);

        /*
         * Append a sublist for each type of proc.
         */

        if (nodePtr->dataInherit != NULL) {
            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppend(dsPtr, subDs.string, subDs.length);
            Tcl_DStringAppendElement(dsPtr, "inherit");
            //fprintf(stderr, "... node %p call callback for dataInherit\n", (void*)nodePtr);
            (*func)(dsPtr, nodePtr->dataInherit);
            Tcl_DStringEndSublist(dsPtr);
        }
        if (nodePtr->dataNoInherit != NULL) {
            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppend(dsPtr, subDs.string, subDs.length);
            Tcl_DStringAppendElement(dsPtr, "noinherit");
            //fprintf(stderr, "... node %p call callback for noInherit\n", (void*)nodePtr);
            (*func)(dsPtr, nodePtr->dataNoInherit);
            Tcl_DStringEndSublist(dsPtr);
        }
        {
            const Ns_Index* indexPtr = &nodePtr->data;
            for (i = 0; i < indexPtr->n; i++) {
                Ns_IndexContextSpec *spec = Ns_IndexEl(indexPtr, i);

                Tcl_DStringStartSublist(dsPtr);
                Tcl_DStringAppend(dsPtr, subDs.string, subDs.length);
                Tcl_DStringAppendElement(dsPtr, "inherit");
                //fprintf(stderr, "...... wanna add [%ld]\n", i);
                (void) NsUrlSpaceContextSpecAppend(dsPtr, (NsUrlSpaceContextSpec*)spec);
                (*func)(dsPtr, spec->data);
                Tcl_DStringEndSublist(dsPtr);
            }
        }

        Tcl_DStringFree(&subDs);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NodeDestroy --
 *
 *      Free a node and its data.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The delete function is called and the node is freed.
 *
 *----------------------------------------------------------------------
 */

static void
NodeDestroy(Node *nodePtr)
{
    NS_NONNULL_ASSERT(nodePtr != NULL);

    if (nodePtr->deletefuncNoInherit != NULL) {
        (*nodePtr->deletefuncNoInherit)(nodePtr->dataNoInherit);
    }
    if (nodePtr->deletefuncInherit != NULL) {
        (*nodePtr->deletefuncInherit)(nodePtr->dataInherit);
    }
#ifdef CONTEXT_FILTER
    ContextFilterDestroy(&nodePtr->data);
    Ns_IndexDestroy(&nodePtr->data);
    //fprintf(stderr, "...   NodeDestory destroy data %p for node %p\n",
    //        (void*)&nodePtr->data, (void*)nodePtr);
#endif
    ns_free(nodePtr);
}

static void ContextFilterDestroy(const Ns_Index* indexPtr)
{
    size_t i;

    for (i = 0u; i < indexPtr->n; i++) {
        Ns_IndexContextSpec *spec = Ns_IndexEl(indexPtr, i);

        if (spec->dataFreeProc != NULL) {
            (spec->dataFreeProc)(spec->data);
        }
        if (spec->freeProc != NULL) {
            (spec->freeProc)(spec);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * CmpBranches --
 *
 *      Compare two branches' word members. Called by Ns_Index*
 *
 * Results:
 *      0 if equal, -1 if left is greater; 1 if right is greater.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpBranches(const void *leftPtrPtr, const void *rightPtrPtr)
{
    const char *wordLeft, *wordRight;

    NS_NONNULL_ASSERT(leftPtrPtr != NULL);
    NS_NONNULL_ASSERT(rightPtrPtr != NULL);

    wordLeft = (*(const Branch **)leftPtrPtr)->word;
    wordRight = (*(const Branch **)rightPtrPtr)->word;
#ifdef DEBUG
    fprintf(stderr, "CmpBranches '%s' with '%s' -> %d\n", wordLeft, wordRight,
            NS_strcmp(wordLeft, wordRight));
#endif
    return NS_strcmp(wordLeft, wordRight);
}

static int
CmpUrlSpaceContextSpecs(const void *leftPtrPtr, const void *rightPtrPtr)
{
    const UrlSpaceContextSpec *ctxLeft, *ctxRight;
    int result = 0;

    ctxLeft = *(UrlSpaceContextSpec **)leftPtrPtr;
    ctxRight = *(UrlSpaceContextSpec **)rightPtrPtr;


    if (ctxLeft->type != ctxRight->type) {
        if (ctxLeft->type == 'h') {
            result = 1;
        } else if (ctxRight->type == 'h') {
            result = -1;
        } else if (ctxLeft->type == '4') {
            result = 1;
        } else if (ctxRight->type == '4') {
            result = -1;
        } else {
            fprintf(stderr, "================== how comes left <%c %s> right <%c %s>\n",
                    ctxLeft->type, ctxLeft->patternString,
                    ctxRight->type, ctxRight->patternString);
        }
    } else {
        result = 0;
    }

    if (result == 0) {
        bool leftHasPattern, rightHasPattern;

        /* Both sides have the same types */

        leftHasPattern = ctxLeft->hasPattern;
        rightHasPattern = ctxRight->hasPattern;
        if (leftHasPattern && rightHasPattern) {
            /*
             * Both have a wildcard, take the longer pattern as more concrete.
             */
            result = ((int)ctxRight->specificity - (int)ctxLeft->specificity);
            if (result == 0) {
                /*
                 * Both patterns are equally long -> take lexical order.
                 */
                result = NS_strcmp(ctxLeft->patternString, ctxLeft->patternString);
            }
        } else if (!leftHasPattern && !rightHasPattern){
            /*
             * Both have no wildcard -> take lexical order
             */
            result = NS_strcmp(ctxLeft->patternString, ctxLeft->patternString);
        } else {
            /*
             * Pattern with no wildcard is more concrete.
             */
            result = leftHasPattern - rightHasPattern;
        }
    }
#if 0 || defined(DEBUG)
    fprintf(stderr, "CmpContextFilters '%s: %s' (%c %u) with '%s: %s' (%c %u) -> %d\n",
            ctxLeft->field, ctxLeft->patternString, ctxLeft->type, ctxLeft->specificity,
            ctxRight->field, ctxRight->patternString, ctxRight->type, ctxRight->specificity,
            result);
#endif
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithBranch --
 *
 *      Compare a branch's word to a passed-in key; called by
 *      Ns_Index*.
 *
 * Results:
 *      0 if equal, -1 if left is greater; 1 if right is greater.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpKeyWithBranch(const void *key, const void *elemPtr)
{
    const char *keyString, *word;

    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(elemPtr != NULL);

    keyString = (const char *)key;
    word = (*(const Branch **)elemPtr)->word;

#ifdef DEBUG
    fprintf(stderr, "CmpKeyWithBranch '%s' with '%s' -> %d\n",
            keyString, word, NS_strcmp(keyString, word));
#endif
    return NS_strcmp(keyString, word);
}

static int
CmpKeyWithUrlSpaceContextSpecs(const void *key, const void *elemPtr)
{
    const char *keyString, *word;

    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(elemPtr != NULL);

    keyString = (const char *)key;
    word = (*(const UrlSpaceContextSpec **)elemPtr)->patternString;

#if 1 || defined(DEBUG)
    fprintf(stderr, "CmpKeyWithUrlSpaceContextSpecs '%s' with '%s' -> %d\n",
            keyString, word, NS_strcmp(keyString, word));
#endif
    return NS_strcmp(keyString, word);
}

/*
 *----------------------------------------------------------------------
 *
 * BranchDestroy --
 *
 *      Free a branch structure.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free memory.
 *
 *----------------------------------------------------------------------
 */

static void
BranchDestroy(Branch *branchPtr)
{
    ns_free(branchPtr->word);
    TrieDestroy(&branchPtr->trie);
    ns_free(branchPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * TrieInit --
 *
 *      Initialize a Trie data structure with 25 branches and set the
 *      Cmp functions for Ns_Index*.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The trie is initialized and memory is allocated.
 *
 *----------------------------------------------------------------------
 */

static void
TrieInit(Trie *triePtr)
{
    NS_NONNULL_ASSERT(triePtr != NULL);

    Ns_IndexInit(&triePtr->branches, 25u, CmpBranches, CmpKeyWithBranch);
    triePtr->node = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * TrieAdd --
 *
 *      Add something to a Trie data structure.
 *
 *      seq is a null-delimited string of words, terminated with
 *      two nulls.
 *      id is allocated with Ns_UrlSpecificAlloc.
 *      flags is a bit mask of NS_OP_NODELETE, NS_OP_NOINHERIT for
 *      desired behavior.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Memory is allocated. If a node is found and the
 *      NS_OP_NODELETE is not set, the current node's data is deleted.
 *
 *----------------------------------------------------------------------
 */

static void
TrieAdd(Trie *triePtr, char *seq, void *data, unsigned int flags,
        Ns_FreeProc deleteProc, void *contextSpec)
{
    NS_NONNULL_ASSERT(triePtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);
    NS_NONNULL_ASSERT(data != NULL);

#ifdef DEBUG
    fprintf(stderr, "...   TrieAdd '%s' contextSpec %p\n", seq, contextSpec);
#endif
    if (*seq != '\0') {
        Branch *branchPtr;

        /*
         * We are still parsing the middle of a sequence, such as
         * "foo" in: "server1\0GET\0foo\0*.html\0"
         *
         * Create a new branch and recurse to add the next word in the
         * sequence.
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
#ifdef DEBUG
        fprintf(stderr, "--- locate '%s' in trie %p %d => %p\n",
                seq, (void*)triePtr, triePtr->branches.CmpEls == CmpBranches, (void*)branchPtr);
#endif
        if (branchPtr == NULL) {
            branchPtr = ns_malloc(sizeof(Branch));
            branchPtr->word = ns_strdup(seq);
            TrieInit(&branchPtr->trie);

            Ns_IndexAdd(&triePtr->branches, branchPtr);
#ifdef DEBUG
            fprintf(stderr, "--- Ns_IndexAdd adds branch %p for '%s' (size %lu) \n",
                    (void*)branchPtr, seq, triePtr->branches.n);
#endif
        }
        TrieAdd(&branchPtr->trie, seq + NS_strlen(seq) + 1u, data, flags,
                deleteProc, contextSpec);

    } else {
        Node *nodePtr;

        /*
         * The entire sequence has been traversed, creating a branch
         * for each word. Now it is time to make a Node.
         */
#ifdef DEBUG
        fprintf(stderr, "...   TrieAdd '%s' end of traversal (n %lu)\n", seq, triePtr->branches.n);
#endif
        //nodePtr = ns_calloc(1u, sizeof(Node));
        //Ns_IndexAdd(&triePtr->branches, branchPtr);

#ifdef DEBUG
        fprintf(stderr, "...   TrieAdd '%s' n %lu max %ld, inc %ld cmpKey %d, trie %p\n",
                seq,
                triePtr->branches.n,
                triePtr->branches.max,
                triePtr->branches.inc,
                triePtr->branches.CmpEls == CmpBranches,
                (void*)triePtr);
#endif
        if (triePtr->node == NULL) {

            nodePtr = ns_calloc(1u, sizeof(Node));
#ifdef DEBUG
            fprintf(stderr, "...   TrieAdd '%s' alloc new node (n %lu) %p\n",
                    seq, triePtr->branches.n, (void*)nodePtr);
#endif
#ifdef CONTEXT_FILTER
            Ns_IndexInit(&nodePtr->data, 10u, CmpUrlSpaceContextSpecs, CmpKeyWithUrlSpaceContextSpecs);
#endif
            triePtr->node = nodePtr;
        } else {

            /*
             * If NS_OP_NODELETE is NOT set, then delete the current
             * node because one already exists.
             */

            nodePtr = triePtr->node;
            if ((flags & NS_OP_NODELETE) == 0u) {
                if ((flags & NS_OP_NOINHERIT) != 0u) {
                    Ns_Log(Ns_LogUrlspaceDebug,
                           "...   TrieAdd '%s' delete node NOINHERIT %p",
                           seq, (void*)nodePtr);
                    if (nodePtr->deletefuncNoInherit != NULL) {
                        (*nodePtr->deletefuncNoInherit)(nodePtr->dataNoInherit);
                    }
                } else {
                    bool freeOldData = NS_TRUE;
#ifdef CONTEXT_FILTER
                    if (contextSpec != NULL) {
                        freeOldData = NS_FALSE;
                    }
#endif
                    if (freeOldData && nodePtr->deletefuncInherit != NULL) {
                        Ns_Log(Ns_LogUrlspaceDebug,
                               "...   TrieAdd '%s' delete node INHERIT %p",
                               seq, (void*)nodePtr);

                        (*nodePtr->deletefuncInherit)(nodePtr->dataInherit);
                        nodePtr->dataInherit = NULL;
                    }
                }
            }
        }
        if ((flags & NS_OP_NOINHERIT) != 0u) {
            nodePtr->dataNoInherit = data;
            nodePtr->deletefuncNoInherit = deleteProc;
        } else if (contextSpec == NULL) {
            nodePtr->dataInherit = data;
            nodePtr->deletefuncInherit = deleteProc;
        }
#ifdef CONTEXT_FILTER
        if (contextSpec != NULL && (flags & NS_OP_NOINHERIT) == 0u)  {
            Ns_IndexContextSpec *spec = contextSpec;

            spec->data = data;
            spec->dataFreeProc = deleteProc;

            Ns_IndexAdd(&nodePtr->data, spec);
            Ns_Log(Ns_LogUrlspaceDebug,
                    "...   TrieAdd '%s' new %p added to trie %p size now %" PRIuz,
                    seq, (void*)nodePtr, (void*)triePtr, nodePtr->data.n);
        }
#endif
        //fprintf(stderr, "...   TrieAdd '%s' configure done trie %p node %p\n",
        //        seq, (void*)triePtr, (void*)nodePtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * TrieTrunc --
 *
 *      Remove all nodes from a trie.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Nodes may be destroyed/freed.
 *
 *----------------------------------------------------------------------
 */

static void
TrieTrunc(Trie *triePtr)
{
    Branch *branchPtr;
    size_t  n;

    NS_NONNULL_ASSERT(triePtr != NULL);

    n = Ns_IndexCount(&triePtr->branches);
    if (n > 0u) {
        size_t i;

        /*
         * Loop over each branch and recurse.
         */

        for (i = 0u; i < n; i++) {
            branchPtr = Ns_IndexEl(&triePtr->branches, i);
            TrieTrunc(&branchPtr->trie);
        }
    }
    if (triePtr->node != NULL) {
        NodeDestroy(triePtr->node);
        triePtr->node = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * TrieTruncBranch --
 *
 *      Cut off a branch from a trie.
 *
 * Results:
 *      0 on success, -1 on failure.
 *
 * Side effects:
 *      Will delete a branch.
 *
 *----------------------------------------------------------------------
 */

static int
TrieTruncBranch(Trie *triePtr, char *seq)
{
    Branch *branchPtr;
    int     result;

    NS_NONNULL_ASSERT(triePtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    if (*seq != '\0') {
        branchPtr = Ns_IndexFind(&triePtr->branches, seq);

        /*
         * If this sequence exists, recursively delete it; otherwise
         * return an error.
         */

        if (branchPtr != NULL) {
            result = TrieTruncBranch(&branchPtr->trie, seq + NS_strlen(seq) + 1u);
        } else {
            result = -1;
        }
    } else {

        /*
         * The end of the sequence has been reached. Finish up the job
         * and return success.
         */

        TrieTrunc(triePtr);
        result = 0;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * TrieDestroy --
 *
 *      Delete an entire Trie.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will free all the elements of the trie.
 *
 *----------------------------------------------------------------------
 */

static void
TrieDestroy(Trie *triePtr)
{
    size_t n;

    NS_NONNULL_ASSERT(triePtr != NULL);

    n = Ns_IndexCount(&triePtr->branches);
    if (n > 0u) {
        size_t i;

        /*
         * Loop over each branch and delete it
         */

        for (i = 0u; i < n; i++) {
            Branch *branchPtr = Ns_IndexEl(&triePtr->branches, i);
            BranchDestroy(branchPtr);
        }
        Ns_IndexDestroy(&triePtr->branches);
    }
    if (triePtr->node != NULL) {
        NodeDestroy(triePtr->node);
        triePtr->node = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * TrieFind --
 *
 *      Find a node in a trie matching a sequence.
 *
 * Results:
 *      Return the appropriate node's data.
 *
 * Side effects:
 *      The depth variable will be set-by-reference to the depth of
 *      the returned node. If no node is set, it will not be changed.
 *
 *----------------------------------------------------------------------
 */

static void *
TrieFind(const Trie *triePtr, char *seq, NsUrlSpaceContextFilterProc proc, void *context, int *depthPtr)
{
    const Node   *nodePtr;
    const Branch *branchPtr;
    void         *data = NULL;
    int           ldepth;

    NS_NONNULL_ASSERT(triePtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);
    NS_NONNULL_ASSERT(depthPtr != NULL);

    nodePtr = triePtr->node;
    ldepth = *depthPtr;

#ifdef DEBUG
    fprintf(stderr, "...    TrieFind seq '%s' nodePtr %p ldepth %d\n", seq, (void*)nodePtr, ldepth);
#endif

    if (nodePtr != NULL) {
        if (
            (*seq == '\0') /* this makes "set -noinherit /x/ *.html foo" + "get /x/a.html" fail */
            && (nodePtr->dataNoInherit != NULL)) {
            data = nodePtr->dataNoInherit;
        } else {
            data = nodePtr->dataInherit;
#ifdef CONTEXT_FILTER
            if (nodePtr->data.n != 0) {
                /*
                 * We have context filters
                 */
                if (context != NULL) {
                    size_t i;
#ifdef DEBUG
                    fprintf(stderr, "...    TrieFind seq '%s' nodePtr %p context %p context specs %ld\n",
                            seq, (void*)nodePtr, context, nodePtr->data.n);
#endif
                    for (i = 0u; i < nodePtr->data.n; i++) {
                        Ns_IndexContextSpec *spec = Ns_IndexEl(&nodePtr->data, i);
                        bool success;

                        assert(proc != NULL);
                        success = (proc)(spec, context);
#ifdef DEBUG
                        fprintf(stderr, ".......[%ld]    TrieFind seq '%s' nodePtr %p => success %d\n",
                                i, seq, (void*)nodePtr, success);
#endif
                        if (success) {
                            data = spec->data;
                            break;
                        }
                    }
                }
            }
#endif
        }
#ifdef DEBUG
        fprintf(stderr, "...    TrieFind seq '%s' nodePtr %p -> data %p\n", seq, (void*)nodePtr, data);
#endif
    }
    if (*seq != '\0') {

        /*
         * We have not yet reached the end of the sequence, so
         * recurse if there are any sub-branches
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
        ldepth += 1;
#ifdef DEBUG
        fprintf(stderr, "...    TrieFind seq '%s' recurse on branch %p\n", seq, (void*)branchPtr);
#endif
        if (branchPtr != NULL) {
            void *p = TrieFind(&branchPtr->trie, seq + NS_strlen(seq) + 1u, proc, context, &ldepth);
            if (p != NULL) {
                data = p;
                *depthPtr = ldepth;
            }
        }
    }

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * TrieFindExact --
 *
 *      Similar to TrieFind, but will not do inheritance.  If (flags &
 *      NS_OP_NOINHERIT) then data set with that flag will be
 *      returned; otherwise only data set without that flag will be
 *      returned.
 *
 * Results:
 *      See TrieFind.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void *
TrieFindExact(const Trie *triePtr, char *seq, unsigned int flags, Node **nodePtrPtr)
{
    Node         *nodePtr;
    const Branch *branchPtr;
    void         *data = NULL;

    NS_NONNULL_ASSERT(triePtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    nodePtr = triePtr->node;

    if (*seq != '\0') {

        /*
         * We have not reached the end of the sequence yet, so
         * we must recurse.
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
        if (branchPtr != NULL) {
            data = TrieFindExact(&branchPtr->trie, seq + NS_strlen(seq) + 1u, flags, nodePtrPtr);
        }
    } else if (nodePtr != NULL) {

        /*
         * We reached the end of the sequence. Grab the data from this
         * node. If the flag specifies NOINHERIT, then return the
         * non-inheriting data, otherwise return the inheriting data.
         */

        if ((flags & NS_OP_NOINHERIT) != 0u) {
            data = nodePtr->dataNoInherit;
        } else {
            data = nodePtr->dataInherit;
        }
        *nodePtrPtr = nodePtr;
    } else {
        *nodePtrPtr = nodePtr;
    }

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * TrieDelete --
 *
 *      Delete a URL, defined by a sequence, from a trie.
 *
 *      The NS_OP_NOINHERIT bit may be set in flags to use
 *      non-inheriting data; NS_OP_NODELETE may be set to
 *      skip calling the delete function.
 *
 * Results:
 *      A pointer to the now-deleted data.
 *
 * Side effects:
 *      Data may be deleted.
 *
 *----------------------------------------------------------------------
 */

static void *
TrieDelete(const Trie *triePtr, char *seq, unsigned int flags)
{
    Node         *nodePtr;
    const Branch *branchPtr;
    void         *data = NULL;

    NS_NONNULL_ASSERT(triePtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    nodePtr = triePtr->node;
    Ns_Log(Ns_LogUrlspaceDebug, "TrieDelete %s 0x%.6x", seq, flags);

    if (*seq != '\0') {

        /*
         * We have not yet reached the end of the sequence. So
         * recurse.
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
        if (branchPtr != NULL) {
            data = TrieDelete(&branchPtr->trie, seq + NS_strlen(seq) + 1u, flags);
        }
    } else if (nodePtr != NULL) {

        /*
         * We've reached the end of the sequence; if a node exists for
         * this ID then delete the inheriting/non-inheriting data (as
         * specified in flags) and call the delete func if requested.
         * The data will be set to null either way.
         */

        if ((flags & NS_OP_NOINHERIT) != 0u) {
            data = nodePtr->dataNoInherit;
            nodePtr->dataNoInherit = NULL;
            if (nodePtr->deletefuncNoInherit != NULL) {
                if ((flags & NS_OP_NODELETE) == 0u) {
                    (*nodePtr->deletefuncNoInherit) (data);
                }
                nodePtr->deletefuncNoInherit = NULL;
            }
        } else {
            data = nodePtr->dataInherit;
            nodePtr->dataInherit = NULL;
            if (nodePtr->deletefuncInherit != NULL) {
                if ((flags & NS_OP_NODELETE) == 0u) {
                    (*nodePtr->deletefuncInherit) (data);
                }
                nodePtr->deletefuncInherit = NULL;
            }
        }
#ifdef CONTEXT_FILTER
        /*
         * When NS_OP_ALLFILTERS is set, then delete all filters.
         * TODO: selective filter deletion not implemented.
         */
        if ((flags & NS_OP_ALLFILTERS) != 0u) {
            Ns_Index* indexPtr = &nodePtr->data;

            //fprintf(stderr, "...   TrieTele NS_OP_ALLFILTERS data %p for node %p fn %p n %" PRIuz "\n",
            //        (void*)&indexPtr, (void*)nodePtr, (void*)indexPtr->CmpEls, indexPtr->n);

            ContextFilterDestroy(indexPtr);
            Ns_IndexTrunc(indexPtr);
        }

#endif
    }

    return data;
}

#ifndef __URLSPACE_OPTIMIZE__

/*
 *----------------------------------------------------------------------
 *
 * CmpChannels --
 *
 *      Compare the filters of two channels.
 *
 * Results:
 *      0: Not the case that one contains the other OR they both
 *      contain each other; 1: left contains right; -1: right contains
 *      left.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpChannels(const void *leftPtrPtr, const void *rightPtrPtr)
{
    const char *filterLeft, *filterRight;
    bool       lcontainsr, rcontainsl;
    int        result;

    NS_NONNULL_ASSERT(leftPtrPtr != NULL);
    NS_NONNULL_ASSERT(rightPtrPtr != NULL);

    filterLeft = (*(const Channel **)leftPtrPtr)->filter;
    filterRight = (*(const Channel **)rightPtrPtr)->filter;

    lcontainsr = NS_Tcl_StringMatch(filterRight, filterLeft);
    rcontainsl = NS_Tcl_StringMatch(filterLeft, filterRight);

    if (lcontainsr && rcontainsl) {
        result = 0;
    } else if (lcontainsr) {
        result = 1;
    } else if (rcontainsl) {
        result = -1;
    } else {
        result = 0;
    }

#ifdef DEBUG
    fprintf(stderr, "======= CmpChannels '%s' <> '%s' -> %d\n",
            filterLeft, filterRight, result);
#endif

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithChannel --
 *
 *      Compare a key to a channel's filter.
 *
 * Results:
 *      0: Not the case that one contains the other OR they both
 *      contain each other; 1: key contains filter; -1: filter
 *      contains key.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CmpKeyWithChannel(const void *key, const void *elemPtr)
{
    const char *filter;
    int         lcontainsr, rcontainsl, result;

    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(elemPtr != NULL);

    filter = (*(const Channel **)elemPtr)->filter;

    lcontainsr = NS_Tcl_StringMatch(filter, key);
    rcontainsl = NS_Tcl_StringMatch(key, filter);
    if (lcontainsr != 0 && rcontainsl != 0) {
        result = 0;
    } else if (lcontainsr != 0) {
        result = 1;
    } else if (rcontainsl != 0) {
        result = -1;
    } else {
        result = 0;
    }

#ifdef DEBUG
    fprintf(stderr, "======= CmpKeyWithChannel %s with %s -> %d\n", filter, key, result);
#endif
    return result;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * CmpChannelsAsStrings --
 *
 *      Compare the filters of two channels.
 *
 * Results:
 *      Same as NS_strcmp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpChannelsAsStrings(const void *leftPtrPtr, const  void *rightPtrPtr)
{
    const char *filterLeft, *filterRight;

    NS_NONNULL_ASSERT(leftPtrPtr != NULL);
    NS_NONNULL_ASSERT(rightPtrPtr != NULL);

    filterLeft = (*(const Channel **)leftPtrPtr)->filter;
    filterRight = (*(const Channel **)rightPtrPtr)->filter;

#ifdef DEBUG
    fprintf(stderr, "CmpChannelsAsStrings '%s' with '%s' -> %d\n",
            filterLeft, filterRight,
            NS_strcmp(filterLeft, filterRight));
#endif
    return NS_strcmp(filterLeft, filterRight);
}


/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithChannelAsStrings --
 *
 *      Compare a string key to a channel's filter
 *
 * Results:
 *      Same as NS_strcmp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpKeyWithChannelAsStrings(const void *key, const void *elemPtr)
{
    const char *filter;

    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(elemPtr != NULL);

    filter = (*(const Channel **)elemPtr)->filter;

#ifdef DEBUG
    fprintf(stderr, "CmpKeyWithChannelAsStrings key '%s' with '%s' -> %d\n",
            key, filter, NS_strcmp(key, filter));
#endif

    return NS_strcmp(key, filter);
}


/*
 *----------------------------------------------------------------------
 *
 * GetJunction --
 *
 *      Get the junction corresponding to the given server and id.
 *      Ns_UrlSpecificAlloc() must have already been called.
 *
 * Results:
 *      Pointer to junction.
 *
 * Side effects:
 *      Will initialize the junction on first access.
 *
 *----------------------------------------------------------------------
 */

static Junction *
JunctionGet(NsServer *servPtr, int id)
{
    Junction *juncPtr;

    NS_NONNULL_ASSERT(servPtr != NULL);

    juncPtr = servPtr->urlspace.junction[id];
    if (juncPtr == NULL) {
        juncPtr = ns_malloc(sizeof *juncPtr);
#ifndef __URLSPACE_OPTIMIZE__
        Ns_IndexInit(&juncPtr->byuse, 5u, CmpChannels, CmpKeyWithChannel);
#endif
        Ns_IndexInit(&juncPtr->byname, 5u,
                     CmpChannelsAsStrings, CmpKeyWithChannelAsStrings);
        servPtr->urlspace.junction[id] = juncPtr;
    }

    assert(juncPtr != NULL);

    return juncPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * JunctionTruncBranch --
 *
 *      Truncate a branch within a junction, given a sequence.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See TrieTruncBranch.
 *
 *----------------------------------------------------------------------
 */

static void
JunctionTruncBranch(const Junction *juncPtr, char *seq)
{
    Channel *channelPtr;
    size_t   i, n;

    NS_NONNULL_ASSERT(juncPtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    /*
     * Loop over every channel in a junction and truncate the sequence in
     * each.
     */
#ifndef __URLSPACE_OPTIMIZE__
    n = Ns_IndexCount(&juncPtr->byuse);
    for (i = 0u; i < n; i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
        (void) TrieTruncBranch(&channelPtr->trie, seq);
    }
#else
    n = Ns_IndexCount(&juncPtr->byname);
    for (i = n; i > 0u; i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i - 1u);
        (void) TrieTruncBranch(&channelPtr->trie, seq);
    }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * JunctionAdd --
 *
 *      This function is called from Ns_UrlSpecificSet which is
 *      usually called from Ns_RegisterRequest,
 *      Ns_RegisterProxyRequest, InitAliases for mapping aliases, and
 *      the nsperm functions TribeAlloc and Ns_AddPermission for
 *      adding permissions. It adds a sequence, terminating in a new
 *      node, to a junction.
 *
 *      Flags may be a bit-combination of NS_OP_NOINHERIT,
 *      NS_OP_NODELETE.  NOINHERIT sets the data as non-inheriting, so
 *      only an exact sequence will match in the future; NODELETE
 *      means that if a node already exists with this sequence/ID it
 *      will not be deleted but replaced.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies seq, assuming
 *      seq = "handle\0method\0urltoken\0urltoken\0..\0\0\"
 *
 *----------------------------------------------------------------------
 */

static void
JunctionAdd(Junction *juncPtr, char *seq, void *data, unsigned int flags,
            Ns_FreeProc freeProc,
            void *contextSpec)
{
    Channel    *channelPtr;
    Ns_DString  dsFilter;
    char       *p;
    int         depth;
    size_t      l;

    NS_NONNULL_ASSERT(juncPtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    //fprintf(stderr, "...   JunctionAdd '%s' contextSpec %p\n", seq, contextSpec);

    depth = 0;
    Ns_DStringInit(&dsFilter);

    /*
     * Find out how deep the sequence is, and position p at the
     * beginning of the last word in the sequence.
     */

    for (p = seq; p[l = NS_strlen(p) + 1u] != '\0'; p += l) {
        depth++;
    }
    //fprintf(stderr, "...   JunctionAdd '%s' last word '%s' contextSpec %p\n", seq, p, contextSpec);

    /*
     * If it is a valid sequence that has a wildcard in its last
     * element, append the whole string to dsWord, then cut off the
     * last word from p.
     *
     * Otherwise, set dsWord to "*" because there is an implicit *
     * wildcard at the end of URLs like /foo/bar
     *
     * dsWord will eventually be used to set or find&reuse a channel
     * filter.
     */
    if ((depth > 0) && (strchr(p, INTCHAR('*')) != NULL || strchr(p, INTCHAR('?')) != NULL )) {
                Ns_DStringAppend(&dsFilter, p);
        *p = '\0';
    } else {
        Ns_DStringAppend(&dsFilter, "*");
    }

    /*
     * Find a channel whose filter matches what the filter on this URL
     * should be.
     */

    channelPtr = Ns_IndexFind(&juncPtr->byname, dsFilter.string);
#ifdef DEBUG
        fprintf(stderr, "--- Ns_IndexFind '%s' (size %lu) returned %p\n",
            dsFilter.string, juncPtr->byname.n, (void *)channelPtr);
#endif

    /*
     * If no channel is found, create a new channel and add it to the
     * list of channels in the junction.
     */

    if (channelPtr == NULL) {
        channelPtr = ns_malloc(sizeof(Channel));
        channelPtr->filter = ns_strdup(dsFilter.string);
        TrieInit(&channelPtr->trie);

#ifndef __URLSPACE_OPTIMIZE__
        Ns_IndexAdd(&juncPtr->byuse, channelPtr);
        //fprintf(stderr, "--- Ns_IndexAdd for channel by use '%s' (size %lu) \n",
        //        channelPtr->filter, juncPtr->byuse.n);
#endif
        Ns_IndexAdd(&juncPtr->byname, channelPtr);
        //fprintf(stderr, "--- Ns_IndexAdd for channel by name '%s' (size %lu) \n",
        //        channelPtr->filter, juncPtr->byname.n);

    }
    Ns_DStringFree(&dsFilter);

    /*
     * Now we need to create a sequence of branches in the trie (if no
     * appropriate sequence already exists) and a node at the end of it.
     * TrieAdd will do that.
     */

    TrieAdd(&channelPtr->trie, seq, data, flags, freeProc, contextSpec);
}


/*
 *----------------------------------------------------------------------
 *
 * JunctionFind --
 *
 *      Locate a node for a given sequence in a junction.
 *      As usual sequence is "method\0urltoken\0...\0\0".
 *
 *      The "fast" boolean switch makes it do NS_strcmp instead of
 *      Tcl string matches on the filters. Not useful for wildcard
 *      matching.
 *
 * Results:
 *      User data.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void *
JunctionFind(const Junction *juncPtr, char *seq, NsUrlSpaceContextFilterProc proc, void *context)
{
    const Channel *channelPtr;
    const char    *p;
    size_t         i, l;
    int            depth = 0;
    void          *data;

    NS_NONNULL_ASSERT(juncPtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    /*
     * After this loop, p will point at the last element in the
     * sequence.
     */

    for (p = seq; p[l = NS_strlen(p) + 1u] != '\0'; p += l) {
        ;
    }

    /*
     * Check filters from most restrictive to least restrictive
     */

    data = NULL;
#ifndef __URLSPACE_OPTIMIZE__
    l = Ns_IndexCount(&juncPtr->byuse);
#else
    l = Ns_IndexCount(&juncPtr->byname);
#endif

    if (l == 0u) {
        return NULL;
    }

    /*
     * For __URLSPACE_OPTIMIZE__
     * Basically if we use the optimize, let's reverse the order
     * by which we search because the byname is in "almost" exact
     * reverse lexicographical order.
     *
     * Loop over all the channels in the index.
     */

#ifndef __URLSPACE_OPTIMIZE__
    for (i = 0u; i < l; i++) {
        bool doit;

        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    for (i = l; i > 0u; i--) {
        bool doit;

        channelPtr = Ns_IndexEl(&juncPtr->byname, i - 1u);
#endif

        doit = (
                (*(channelPtr->filter) == '*' && *(channelPtr->filter + 1) == '\0')
                || (NS_Tcl_StringMatch(p, channelPtr->filter) == 1)
                );

#ifdef DEBUG
        fprintf(stderr, "JunctionFind: compare filter '%s' with channel filter '%s' => %d\n",
                p, channelPtr->filter, doit);
#endif
        if (doit) {
            /*
             * We got here because this URL matches the filter
             * (for example, "*.adp").
             */

            if (data == NULL) {
                /*
                 * Nothing has been found so far. Traverse the channel
                 * and find the node; set data to that. Depth will be
                 * set to the level of the node.
                 */

                depth = 0;
                data = TrieFind(&channelPtr->trie, seq, proc, context, &depth);
            } else {
                void *candidate;
                int   cdepth;

                /*
                 * Let's see if this channel has a node that also
                 * matches the sequence but is more specific (has a
                 * greater depth) that the previously found node.
                 */

                cdepth = 0;
                candidate = TrieFind(&channelPtr->trie, seq, proc, context, &cdepth);
                if ((candidate != NULL) && (cdepth > depth)) {
                    data = candidate;
                    depth = cdepth;
                }
            }
        }

#ifdef DEBUG
        if (depth > 0) {
            if (data == NULL) {
                fprintf(stderr, "Channel %s: No match\n", channelPtr->filter);
            } else {
                fprintf(stderr, "Channel %s: depth=%d, data=%p\n",
                        channelPtr->filter, depth, data);
            }
        }
#endif
    }

#ifdef DEBUG
    if (depth > 0) {
        fprintf(stderr, "Done.\n");
    }
#endif

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * JunctionFindExact --
 *
 *      Find a node in a junction that exactly matches a sequence.
 *
 * Results:
 *      User data.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void *
JunctionFindExact(const Junction *juncPtr, char *seq, unsigned int flags)
{
    const Channel *channelPtr;
    char          *p;
    size_t         l, i;
    void          *data = NULL;
    Node          *nodePtr;

    NS_NONNULL_ASSERT(juncPtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    /*
     * Set p to the last element of the sequence.
     */

    for (p = seq; p[l = NS_strlen(p) + 1u] != '\0'; p += l) {
        ;
    }

    /*
     * First, loop through all the channels that have non-"*"
     * filters looking for an exact match
     */

#ifndef __URLSPACE_OPTIMIZE__
    l = Ns_IndexCount(&juncPtr->byuse);
#else
    l = Ns_IndexCount(&juncPtr->byname);
#endif
    if (l == 0u) {
        goto done;
    }

#ifndef __URLSPACE_OPTIMIZE__
    for (i = 0u; i < l; i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    for (i = l; i > 0u; i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i - 1u);
#endif
        if (STREQ(p, channelPtr->filter)) {

            /*
             * The last element of the sequence exactly matches the
             * filter, so this is the one. Wipe out the last word and
             * return whatever node comes out of TrieFindExact.
             */

            *p = '\0';
            data = TrieFindExact(&channelPtr->trie, seq, flags, &nodePtr);
            goto done;
        }
    }

    /*
     * Now go to the channel with the "*" filter and look there for
     * an exact match:
     */

#ifndef __URLSPACE_OPTIMIZE__
    for (i = 0u; i < l; i++) {
      channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    for (i = l; i > 0u; i--) {
      channelPtr = Ns_IndexEl(&juncPtr->byname, i - 1u);
#endif
      if (*(channelPtr->filter) == '*' && *(channelPtr->filter + 1) == '\0') {
          data = TrieFindExact(&channelPtr->trie, seq, flags, &nodePtr);
            break;
        }
    }

    done:

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * JunctionDeleteNode --
 *
 *      Delete a node from a junction matching a sequence
 *
 * Results:
 *      A pointer to the deleted node
 *
 * Side effects:
 *      Seq will be modified.
 *      The node will be deleted if NS_OP_NODELETE isn't set in flags.
 *
 *----------------------------------------------------------------------
 */

static void *
JunctionDeleteNode(const Junction *juncPtr, char *seq, unsigned int flags)
{
    const Channel *channelPtr;
    char          *p;
    size_t         i, l;
    /*int          depth = 0;*/
    void          *data = NULL;

    NS_NONNULL_ASSERT(juncPtr != NULL);
    NS_NONNULL_ASSERT(seq != NULL);

    /*
     * Set p to the last element of the sequence, and
     * depth to the number of elements in the sequence.
     */

    for (p = seq; p[l = NS_strlen(p) + 1u] != '\0'; p += l) {
        /*depth++;*/
        ;
    }

#ifndef __URLSPACE_OPTIMIZE__
    l = Ns_IndexCount(&juncPtr->byuse);
    //Ns_Log(Ns_LogUrlspaceDebug, "JunctionDeleteNode %s 0x%.6x IndexCount %lu", p, flags, l);

    for (i = 0u; (i < l) && (data == NULL); i++) {
        Node *nodePtr = NULL;

        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    l = Ns_IndexCount(&juncPtr->byname);
    for (i = l; (i > 0u) && (data == NULL); i--) {
        Node *nodePtr = NULL;

        channelPtr = Ns_IndexEl(&juncPtr->byname, i - 1u);
#endif
        //Ns_Log(Ns_LogUrlspaceDebug, "JunctionDeleteNode %s 0x%.6x cmp <%s> <%s>", p, flags, p, channelPtr->filter);

        if (
            /* (depth == 2) && */
            STREQ(p, channelPtr->filter)
            ) {
            /*
             * This filter exactly matches the last element of the
             * sequence, so get the node and delete it. (This is
             * server-specific data because depth is 2).
             */

            *p = '\0';
            data = TrieFindExact(&channelPtr->trie, seq, flags, &nodePtr);
            //Ns_Log(Ns_LogUrlspaceDebug, "JunctionDeleteNode %s 0x%.6x cmp find exact -> %p nodePtr %p",
            //       p, flags, (void*)data, (void*)nodePtr);

            if (data != NULL || nodePtr != NULL) {
                (void) TrieDelete(&channelPtr->trie, seq, flags);
            }
        } else if (NS_Tcl_StringMatch(p, channelPtr->filter) == 1) {
            /*
             * The filter matches, so get the node and delete it.
             */

            data = TrieFindExact(&channelPtr->trie, seq, flags, &nodePtr);
            if (data != NULL || nodePtr != NULL) {
                (void) TrieDelete(&channelPtr->trie, seq, flags);
            }
        }
    }

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * MkSeq --
 *
 *      Build a "sequence" out of a method/url; turns it into
 *      "method\0urltoken\0...\0\0".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sequence goes into ds.
 *
 *----------------------------------------------------------------------
 */

static void
MkSeq(Ns_DString *dsPtr, const char *method, const char *url)
{
    const char *p;
    bool        done;
    size_t      l;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Ns_DStringNAppend(dsPtr, method, (int)NS_strlen(method) + 1);

    /*
     * Loop over each directory in the URL and turn the slashes
     * into nulls.
     */

    done = NS_FALSE;
    while (!done && *url != '\0') {
        if (*url != '/') {
            p = strchr(url, INTCHAR('/'));
            if (p != NULL) {
                l = (size_t)(p - url);
            } else {
                l = NS_strlen(url);
                done = NS_TRUE;
            }

            Ns_DStringNAppend(dsPtr, url, (int)l++);
            Ns_DStringNAppend(dsPtr, "\0", 1);
            url += l;
        } else {
            url++;
        }
    }

    /*
     * Put another null on the end to mark the end of the
     * string.
     */

    Ns_DStringNAppend(dsPtr, "\0", 1);
}

#ifdef DEBUG

/*
 *----------------------------------------------------------------------
 *
 * PrintSeq --
 *
 *      Print a null-delimited sequence to stderr.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will write to stderr.
 *
 *----------------------------------------------------------------------
 */

static void
PrintSeq(const char *seq)
{
    const char *p;

    fprintf(stderr, "PrintSeq: <");
    for (p = seq; *p != '\0'; p += NS_strlen(p) + 1u) {
        if (p != seq) {
            fputs(", ", stderr);
        }
        fputs(p, stderr);
    }
    fprintf(stderr, ">\n");

}
#endif


/*
 *----------------------------------------------------------------------
 *
 * AllocTclUrlSpaceId --
 *
 *    Allocate a UrlSpace id for Tcl. It uses the low-level function
 *    Ns_UrlSpecificAlloc() which aborts with Fatal() in case the
 *    server runs out of url spaces. This function does not abort, but
 *    returns a TCL_ERROR in such cases.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Updating the used tclUrlSpaces.
 *
 *----------------------------------------------------------------------
 */

static int
AllocTclUrlSpaceId(Tcl_Interp *interp,  int *idPtr)
{
    int result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(idPtr != NULL);

    if (nextid < MAX_URLSPACES-1) {
        Tcl_DString     ds;
        const NsInterp *itPtr = NsGetInterpData(interp);

        *idPtr =  Ns_UrlSpecificAlloc();
        tclUrlSpaces[*idPtr] = NS_TRUE;

        Tcl_DStringInit(&ds);
        Ns_DStringPrintf(&ds, "ns:rw:urlspace:%d", (int)*idPtr);
        Ns_RWLockInit(&itPtr->servPtr->urlspace.idlocks[*idPtr]);
        Ns_RWLockSetName2(&itPtr->servPtr->urlspace.idlocks[*idPtr], ds.string, itPtr->servPtr->server);
        Tcl_DStringFree(&ds);

        result = TCL_OK;
    } else {
        Ns_TclPrintfResult(interp, "maximum number of urlspaces (%d) reached", MAX_URLSPACES);
        result = TCL_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CheckTclUrlSpaceId --
 *
 *    Either allocate a new UrlSpace id or check, whether the provided
 *    id is an id dedicated "Tcl" (i.e. it is used via ns_urlspace
 *    interface).
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Potentially allocating a new urlspace id.
 *
 *----------------------------------------------------------------------
 */
static int
CheckTclUrlSpaceId(Tcl_Interp *interp, NsServer *servPtr, int *idPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(idPtr != NULL);

    if (*idPtr == -1) {

        Ns_MutexLock(&servPtr->urlspace.lock);
        if (defaultTclUrlSpaceId < 0) {
            /*
             * Allocate a default Tcl urlspace id
             */
            result = AllocTclUrlSpaceId(interp, &defaultTclUrlSpaceId);
        }
        Ns_MutexUnlock(&servPtr->urlspace.lock);

        if (result == TCL_OK) {
            *idPtr = defaultTclUrlSpaceId;
        }

    } else if ((*idPtr < 0) || (*idPtr >= MAX_URLSPACES) || (tclUrlSpaces[*idPtr] == NS_FALSE)) {
        Ns_TclPrintfResult(interp, "provided urlspace id %d is invalid", *idPtr);
        result = TCL_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * WalkCallback --
 *
 *    Callback for Ns_UrlSpecificWalk() used in "ns_urlspace list"
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Appends client data string to provided NS_DString
 *
 *----------------------------------------------------------------------
 */

static void
WalkCallback(Ns_DString *dsPtr, const void *arg)
{
    const char *data = arg;

    Tcl_DStringAppendElement(dsPtr, data);
}




/*
 *----------------------------------------------------------------------
 *
 * UrlSpaceGetObjCmd, subcommand of NsTclUrlSpaceObjCmd --
 *
 *    Implements "ns_urlspace get".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
UrlSpaceGetObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    Ns_Set         *context = NULL;
    int             result = TCL_OK, id = -1;
    char           *key = (char *)".", *url;
    int             exact = (int)NS_FALSE, noinherit = (int)NS_FALSE;
    Ns_ObjvSpec     lopts[] = {
        {"-context",   Ns_ObjvSet,    &context,    NULL},
        {"-exact",     Ns_ObjvBool,   &exact,      INT2PTR(NS_TRUE)},
        {"-id",        Ns_ObjvInt,    &id,        &idRange},
        {"-key",       Ns_ObjvString, &key,        NULL},
        {"-noinherit", Ns_ObjvBool,   &noinherit,  INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"URL",    Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (CheckTclUrlSpaceId(interp, servPtr, &id) != TCL_OK) {
        result = TCL_ERROR;

    } else if (NS_strlen(key) < 1u) {
        Ns_TclPrintfResult(interp, "provided key must be at least one character");
        result = TCL_ERROR;

    } else {
        NsUrlSpaceOp    op;
        unsigned int    flags = 0u;
        NsUrlSpaceContext ctx, *ctxPtr;
        struct NS_SOCKADDR_STORAGE ip;

        if (noinherit == (int)NS_TRUE) {
            exact = (int)NS_TRUE;
        }

        if (exact == (int)NS_TRUE) {
            op = NS_URLSPACE_EXACT;
            if (noinherit) {
                flags |= NS_OP_NOINHERIT;
            }
        } else {
            op = NS_URLSPACE_DEFAULT;
        }
        if (context != NULL) {
            const char *ipString = Ns_SetIGet(context, "X-NS-ip");
            if (ipString != NULL) {
                struct sockaddr *ipPtr = (struct sockaddr *)&ip;
                int validIP = ns_inet_pton(ipPtr, ipString);

                if (validIP > 0) {
                    ctx.saPtr = ipPtr;
                    if (Ns_SetSize(context) > 1) {
                        Ns_TclPrintfResult(interp, "IP has to be in set with a single item");
                        result = TCL_ERROR;
                    }
                } else {
                    Ns_TclPrintfResult(interp, "invalid IP address '%s' specified", ipString);
                    result = TCL_ERROR;
                }
                ctx.headers = NULL;
            } else {
                ctx.headers = context;
            }
            ctxPtr = &ctx;
        } else {
            ctxPtr = NULL;
        }

        if (likely(result == TCL_OK)) {
            const char *data;

#ifdef DEBUG
            fprintf(stderr, "=== GET id %d key %s url %s op %d\n", id, key, url, op);
#endif
            //Ns_Log(Notice, "UrlSpaceGetObjCmd context %p context %p", (void*)context, (void*)ctxPtr);
            Ns_RWLockRdLock(&servPtr->urlspace.idlocks[id]);
            data = NsUrlSpecificGet(servPtr, key, url, id, flags, op, NsUrlSpaceContextFilter, ctxPtr);
            Ns_RWLockUnlock(&servPtr->urlspace.idlocks[id]);

            Tcl_SetObjResult(interp, Tcl_NewStringObj(data, -1));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * UrlSpaceListObjCmd, subcommand of NsTclUrlSpaceObjCmd --
 *
 *    Implements "ns_urlspace list".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
UrlSpaceListObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result = TCL_OK, id = -1;
    Ns_ObjvSpec     lopts[] = {
        {"-id",  Ns_ObjvInt, &id, &idRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (CheckTclUrlSpaceId(interp, servPtr, &id) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString ds, *dsPtr = &ds;

        Ns_DStringInit(dsPtr);

        Ns_RWLockRdLock(&servPtr->urlspace.idlocks[id]);
        Ns_UrlSpecificWalk(id, servPtr->server, WalkCallback, dsPtr);
        Ns_RWLockUnlock(&servPtr->urlspace.idlocks[id]);

        Tcl_DStringResult(interp, dsPtr);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * UrlSpaceNewObjCmd, subcommand of NsTclUrlSpaceObjCmd --
 *
 *    Implements "ns_urlspace new".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
UrlSpaceNewObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        int id = -1;

        Ns_MutexLock(&servPtr->urlspace.lock);
        result = AllocTclUrlSpaceId(interp, &id);
        Ns_MutexUnlock(&servPtr->urlspace.lock);

        if (likely(result == TCL_OK)) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * UrlSpaceSetObjCmd, subcommand of NsTclUrlSpaceObjCmd --
 *
 *    Implements "ns_urlspace set".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
UrlSpaceSetObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result = TCL_OK, id = -1, noinherit = 0, oc = 0;
    char           *key = (char *)".", *url = (char*)NS_EMPTY_STRING, *data = (char*)NS_EMPTY_STRING;
    Tcl_Obj        *headerFilterObj = NULL, **ov = NULL;
    Ns_ObjvSpec     lopts[] = {
        {"-contextfilter", Ns_ObjvObj,    &headerFilterObj, NULL},
        {"-id",            Ns_ObjvInt,    &id,        &idRange},
        {"-key",           Ns_ObjvString, &key,       NULL},
        {"-noinherit",     Ns_ObjvBool,   &noinherit, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"URL",    Ns_ObjvString, &url,    NULL},
        {"data",   Ns_ObjvString, &data,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (CheckTclUrlSpaceId(interp, servPtr, &id) != TCL_OK) {
        result = TCL_ERROR;

    } else if (NS_strlen(key) < 1u) {
        Ns_TclPrintfResult(interp, "provided key must be at least one character");
        result = TCL_ERROR;

    } else if (headerFilterObj != NULL
               && (Tcl_ListObjGetElements(NULL, headerFilterObj, &oc, &ov) != TCL_OK || oc != 2)) {

        Ns_TclPrintfResult(interp,
                           "invalid header filter '%s': must be list containing name and match value",
                           Tcl_GetString(headerFilterObj));
        result = TCL_ERROR;

    } else {
        unsigned int flags = 0u;
        void        *contextSpec = NULL;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }
#ifdef DEBUG
        fprintf(stderr, "=== SET use id %d\n", id);
#endif
        Ns_RWLockWrLock(&servPtr->urlspace.idlocks[id]);

        if (oc == 2) {
            contextSpec = NsUrlSpaceContextSpecNew(Tcl_GetString(ov[0]), Tcl_GetString(ov[1]));
        }
        /* maybe add a non-string interface for first arg */
        //Ns_Log(Ns_LogUrlspaceDebug, "UrlSpaceSetObjCmd contextFilter %p", (void*)contextSpec);
        Ns_UrlSpecificSet2(servPtr->server, key, url, id, ns_strdup(data),
                           flags, ns_free, contextSpec);
        Ns_RWLockUnlock(&servPtr->urlspace.idlocks[id]);
    }
    return result;
}




/*
 *----------------------------------------------------------------------
 *
 * UrlSpaceUnsetObjCmd, subcommand of NsTclUrlSpaceObjCmd --
 *
 *    Implements "ns_urlspace unset".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
UrlSpaceUnsetObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result = TCL_OK, id = -1;
    char           *key = (char *)".", *url;
    int             recurse = (int)NS_FALSE, noinherit = (int)NS_FALSE, allfilters = (int)NS_FALSE;
    Ns_ObjvSpec     lopts[] = {
        {"-allfilters", Ns_ObjvBool,   &allfilters, INT2PTR(NS_TRUE)},
        {"-id",         Ns_ObjvInt,    &id,         &idRange},
        {"-key",        Ns_ObjvString, &key,        NULL},
        {"-noinherit",  Ns_ObjvBool,   &noinherit,  INT2PTR(NS_TRUE)},
        {"-recurse",    Ns_ObjvBool,   &recurse,    INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     args[] = {
        {"URL",    Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (CheckTclUrlSpaceId(interp, servPtr, &id) != TCL_OK) {
        result = TCL_ERROR;

    } else if (NS_strlen(key) < 1u) {
        Ns_TclPrintfResult(interp, "the provided key must contain at least one character");
        result = TCL_ERROR;

    } else {
        const char   *data;
        unsigned int  flags = 0u;

        if (noinherit == (int)NS_TRUE) {
            flags |= NS_OP_NOINHERIT;
        }
        if (allfilters == (int)NS_TRUE) {
            flags |= NS_OP_ALLFILTERS;
        }
        if (recurse == (int)NS_TRUE) {
            flags |= NS_OP_RECURSE;
            if ((flags & NS_OP_NOINHERIT) == NS_OP_NOINHERIT) {
                Ns_Log(Warning, "flag -noinherit is ignored");
            }
        }

        Ns_Log(Ns_LogUrlspaceDebug, "UrlSpaceUnsetObjCmd %s 0x%.6x", url, flags);

        Ns_RWLockWrLock(&servPtr->urlspace.idlocks[id]);
        data = Ns_UrlSpecificDestroy(servPtr->server, key, url, id, flags);
        Ns_RWLockUnlock(&servPtr->urlspace.idlocks[id]);

        Tcl_SetObjResult(interp, Tcl_NewBooleanObj((data != NULL) || recurse));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUrlSpaceObjCmd --
 *
 *    Implements "ns_urlspace".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */

int
NsTclUrlSpaceObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"get",   UrlSpaceGetObjCmd},
        {"list",  UrlSpaceListObjCmd},
        {"new",   UrlSpaceNewObjCmd},
        {"set",   UrlSpaceSetObjCmd},
        {"unset", UrlSpaceUnsetObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
