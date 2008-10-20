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
 *                   0, MyDeleteProc);
 *
 *
 *
 *  NsServer->urlspace: Junction[] [*][ ][ ][ ][ ]
 *                                  |
 *    +-----------------------------+
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

NS_RCSID("@(#) $Header$");


#define STACK_SIZE      512 /* Max depth of URL hierarchy. */


/*
 * This optimization, when turned on, prevents the server from doing a
 * whole lot of calls to Tcl_StringMatch on every lookup in urlspace.
 * Instead, a strcmp is done. This hasn't been thoroughly tested, so
 * it is off by default.
 *
 *  #define __URLSPACE_OPTIMIZE__
 */

/*
 * This structure defines a Node. It is the lowest-level structure in
 * urlspace and contains the data the the user puts in. It holds data
 * whose scope is a set of URLs, such as /foo/bar/ *.html.
 * Data/cleanup functions are kept seperately for inheriting and non-
 * inheriting URLs, as there could be overlap.
 */

typedef struct {
    void  *dataInherit;                      /* User's data */
    void  *dataNoInherit;                    /* User's data */
    void   (*deletefuncInherit) (void *);    /* Cleanup function */
    void   (*deletefuncNoInherit) (void *);  /* Cleanup function */
} Node;

/*
 * This structure defines a trie. A trie is a tree whose nodes are
 * branches and channels. It is an inherently recursive data structure,
 * and each node is itself a trie. Each node represents one "part" of
 * a URL; in this case, a "part" is server name, method, directory, or
 * wildcard.
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
 * branches coming out of this channel (only branches come out of channels).
 * When looking for a URL, the filename part of the target URL is compared
 * with the filter in each channel, and the channel is traversed only if
 * there is a match
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
 * Local functions defined in this file
 */

static void  NodeDestroy(Node *nodePtr);
static void  BranchDestroy(Branch *branchPtr);
static int   CmpBranches(Branch **leftPtrPtr, Branch **rightPtrPtr);
static int   CmpKeyWithBranch(CONST char *key, Branch **branchPtrPtr);

/*
 * Utility functions
 */

static void MkSeq(Ns_DString *dsPtr, CONST char *method, CONST char *url);
static void WalkTrie(Trie *triePtr, Ns_ArgProc func,
                     Ns_DString *dsPtr, char **stack, CONST char *filter);
#ifdef DEBUG
static void PrintSeq(CONST char *seq);
#endif

/*
 * Trie functions
 */

static void  TrieInit(Trie *triePtr);
static void  TrieAdd(Trie *triePtr, char *seq, void *data, int flags, 
                     void (*deletefunc)(void *));
static void *TrieFind(Trie *triePtr, char *seq, int *depthPtr);
static void *TrieFindExact(Trie *triePtr, char *seq, int flags);
static void *TrieDelete(Trie *triePtr, char *seq, int flags);
static void  TrieTrunc(Trie *triePtr);
static int   TrieTruncBranch(Trie *triePtr, char *seq);
static void  TrieDestroy(Trie *triePtr);

/*
 * Channel functions
 */

#ifndef __URLSPACE_OPTIMIZE__
static int CmpChannels(Channel **leftPtrPtr, Channel **rightPtrPtr);
static int CmpKeyWithChannel(CONST char *key, Channel **channelPtrPtr);
#endif

static int CmpChannelsAsStrings(Channel **leftPtrPtr, Channel **rightPtrPtr);
static int CmpKeyWithChannelAsStrings(CONST char *key, Channel **channelPtrPtr);

/*
 * Juntion functions
 */

static Junction *JunctionGet(NsServer *servPtr, int id);
static void JunctionAdd(Junction *juncPtr, char *seq, void *data,
                        int flags, void (*deletefunc)(void *));
static void *JunctionFind(Junction *juncPtr, char *seq, int fast);
static void *JunctionFindExact(Junction *juncPtr, char *seq, int flags,
                               int fast);
static void *JunctionDeleteNode(Junction *juncPtr, char *seq, int flags);
static void JunctionTruncBranch(Junction *juncPtr, char *seq);

/*
 * Static variables defined in this file
 */

/* static Junction urlspace[MAX_URLSPACES]; /\* Junctions keyed by ID. *\/ */


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificAlloc --
 *
 *      Allocate a unique ID to create a seperate virtual URL-space.
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
    static int nextid = 0;
    int        id;

    if ((id = nextid++) >= MAX_URLSPACES) {
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
Ns_UrlSpecificSet(CONST char *server, CONST char *method, CONST char *url, int id,
                  void *data, int flags, void (*deletefunc) (void *))
{
    NsServer   *servPtr = NsGetServer(server);
    Ns_DString  ds;

    if (method == NULL || url == NULL) {
        return;
    }

    Ns_DStringInit(&ds);
    MkSeq(&ds, method, url);
    JunctionAdd(JunctionGet(servPtr, id), ds.string, data, flags, deletefunc);
    Ns_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificGet, Ns_UrlSpecificGetFast --
 *
 *      Find URL-specific data in the subspace identified by id that
 *      the passed-in URL matches.
 *
 *      Ns_UrlSpecificGetFast does not support wild cards.
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
Ns_UrlSpecificGet(CONST char *server, CONST char *method, CONST char *url, int id)
{
    return NsUrlSpecificGet(NsGetServer(server), method, url, id, 0);
}

void *
Ns_UrlSpecificGetFast(CONST char *server, CONST char *method, CONST char *url, int id)
{
    return NsUrlSpecificGet(NsGetServer(server), method, url, id, 1);
}

void *
NsUrlSpecificGet(NsServer *servPtr, CONST char *method, CONST char *url, int id,
                 int fast)
{
    Ns_DString  ds;
    void       *data;

    if (method == NULL || url == NULL) {
        return NULL;
    }

    Ns_DStringInit(&ds);
    MkSeq(&ds, method, url);
    data = JunctionFind(JunctionGet(servPtr, id), ds.string, fast);
    Ns_DStringFree(&ds);

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificGetExact --
 *
 *      Similar to Ns_UrlSpecificGet, but does not support URL
 *      inheritance.
 *
 * Results:
 *      See Ns_UrlSpecificGet.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_UrlSpecificGetExact(CONST char *server, CONST char *method, CONST char *url,
                       int id, int flags)
{
    NsServer   *servPtr = NsGetServer(server);
    Ns_DString  ds;
    void       *data;

    if (method == NULL || url == NULL) {
        return NULL;
    }

    Ns_DStringInit(&ds);
    MkSeq(&ds, method, url);
    data = JunctionFindExact(JunctionGet(servPtr, id), ds.string, flags, 0);
    Ns_DStringFree(&ds);

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlSpecificDestroy --
 *
 *      Delete some urlspecific data.  Flags can be NS_OP_NODELETE,
 *      NS_OP_NOINHERIT, NS_OP_RECURSE.
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
Ns_UrlSpecificDestroy(CONST char *server, CONST char *method, CONST char *url,
                      int id, int flags)
{
    NsServer   *servPtr = NsGetServer(server);
    Ns_DString  ds;
    void       *data = NULL;

    if (method == NULL || url == NULL) {
        return NULL;
    }

    Ns_DStringInit(&ds);
    MkSeq(&ds, method, url);
    if (flags & NS_OP_RECURSE) {
        JunctionTruncBranch(JunctionGet(servPtr, id), ds.string);
        data = NULL;
    } else {
        data = JunctionDeleteNode(JunctionGet(servPtr, id), ds.string, flags);
    }
    Ns_DStringFree(&ds);

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * NsUrlSpecificWalk --
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
Ns_UrlSpecificWalk(int id, CONST char *server, Ns_ArgProc func, Tcl_DString *dsPtr)
{
    Junction *juncPtr;
    Channel  *channelPtr;
    int       n, i;
    char     *stack[STACK_SIZE];

    juncPtr = JunctionGet(NsGetServer(server), id);
    memset(stack, 0, sizeof(stack));

#ifndef __URLSPACE_OPTIMIZE__
    n = Ns_IndexCount(&juncPtr->byuse);
    for (i = 0; i < n; i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    n = Ns_IndexCount(&juncPtr->byname);
    for (i = (n - 1); i >= 0; i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i);
#endif
        WalkTrie(&channelPtr->trie, func, dsPtr, stack, channelPtr->filter);
    }
}
    
static void
WalkTrie(Trie *triePtr, Ns_ArgProc func,
         Ns_DString *dsPtr, char **stack, CONST char *filter)
{
    Branch      *branchPtr;
    Node        *nodePtr;
    int          i, depth;
    Tcl_DString  subDs;

    for (i = 0; i < triePtr->branches.n; i++) {
        branchPtr = Ns_IndexEl(&triePtr->branches, i);

        /*
         * Remember current stack depth
         */

        depth = 0;
        while (stack[depth] != NULL && depth < STACK_SIZE) {
            depth++;
        }
        stack[depth] = branchPtr->word;
        WalkTrie(&branchPtr->trie, func, dsPtr, stack, filter);

        /*
         * Restore stack position
         */

        stack[depth] = 0;
    }

    nodePtr = triePtr->node;
    if (nodePtr != NULL) {

        Tcl_DStringInit(&subDs);

        /*
         * Put stack contents into the sublist.
         * Element 0 is method, the rest is url
         */

        depth = 0;
        Tcl_DStringAppendElement(&subDs, stack[depth++]);
        Tcl_DStringAppend(&subDs, " ", 1);
        if (stack[depth] == NULL) {
            Tcl_DStringAppendElement(&subDs, "/");
        } else {
            while (stack[depth] != NULL) {
                Ns_DStringVarAppend(&subDs, "/", stack[depth], NULL);
                depth++;
            }
        }

        Ns_DStringVarAppend(&subDs, " ", filter, " ", NULL);

        /*
         * Append a sublist for each type of proc.
         */

        if (nodePtr->dataInherit != NULL) {
            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppend(dsPtr, subDs.string, -1);
            Tcl_DStringAppendElement(dsPtr, "inherit");
            (*func)(dsPtr, nodePtr->dataInherit);
            Tcl_DStringEndSublist(dsPtr);
        }
        if (nodePtr->dataNoInherit != NULL) {
            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppend(dsPtr, subDs.string, -1);
            Tcl_DStringAppendElement(dsPtr, "noinherit");
            (*func)(dsPtr, nodePtr->dataNoInherit);
            Tcl_DStringEndSublist(dsPtr);
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
    if (nodePtr->deletefuncNoInherit != NULL) {
        (*nodePtr->deletefuncNoInherit) (nodePtr->dataNoInherit);
    }
    if (nodePtr->deletefuncInherit != NULL) {
        (*nodePtr->deletefuncInherit) (nodePtr->dataInherit);
    }
    ns_free(nodePtr);
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
CmpBranches(Branch **leftPtrPtr, Branch **rightPtrPtr)
{
    return strcmp((*leftPtrPtr)->word, (*rightPtrPtr)->word);
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
CmpKeyWithBranch(CONST char *key, Branch **branchPtrPtr)
{
    return strcmp(key, (*branchPtrPtr)->word);
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
    Ns_IndexInit(&triePtr->branches, 25,
        (int (*) (const void *, const void *)) CmpBranches,
        (int (*) (const void *, const void *)) CmpKeyWithBranch);
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
 *      flags is a bitmask of NS_OP_NODELETE, NS_OP_NOINHERIT for
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
TrieAdd(Trie *triePtr, char *seq, void *data, int flags,
        void (*deletefunc)(void *))
{
    Node   *nodePtr;
    Branch *branchPtr;

    if (*seq != '\0') {

        /*
         * We are still parsing the middle of a sequence, such as "foo" in:
         * "server1\0GET\0foo\0*.html\0"
         *
         * Create a new branch and recurse to add the next word in the
         * sequence.
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
        if (branchPtr == NULL) {
            branchPtr = ns_malloc(sizeof(Branch));
            branchPtr->word = ns_strdup(seq);
            TrieInit(&branchPtr->trie);

            Ns_IndexAdd(&triePtr->branches, branchPtr);
        }
        TrieAdd(&branchPtr->trie, seq + strlen(seq) + 1, data, flags,
                deletefunc);

    } else {

        /*
         * The entire sequence has been traversed, creating a branch
         * for each word. Now it is time to make a Node.
         */

        if (triePtr->node == NULL) {
            triePtr->node = ns_calloc(1, sizeof(Node));
            nodePtr = triePtr->node;
        } else {

            /*
             * If NS_OP_NODELETE is NOT set, then delete the current node
             * because one already exists.
             */

            nodePtr = triePtr->node;
            if ((flags & NS_OP_NODELETE) == 0) {
                if ((flags & NS_OP_NOINHERIT) != 0) {
                    if (nodePtr->deletefuncNoInherit != NULL) {
                        (*nodePtr->deletefuncNoInherit)
                            (nodePtr->dataNoInherit);
                    }
                } else {
                    if (nodePtr->deletefuncInherit != NULL) {
                        (*nodePtr->deletefuncInherit)
                            (nodePtr->dataInherit);
                    }
                }
            }
        }

        if (flags & NS_OP_NOINHERIT) {
            nodePtr->dataNoInherit = data;
            nodePtr->deletefuncNoInherit = deletefunc;
        } else {
            nodePtr->dataInherit = data;
            nodePtr->deletefuncInherit = deletefunc;
        }
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
    int     n, i;

    n = Ns_IndexCount(&triePtr->branches);

    if (n > 0) {

        /*
         * Loop over each branch and recurse.
         */

        for (i = 0; i < n; i++) {
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

    if (*seq != '\0') {
        branchPtr = Ns_IndexFind(&triePtr->branches, seq);

        /*
         * If this sequence exists, recursively delete it; otherwise
         * return an error.
         */

        if (branchPtr != NULL) {
            return TrieTruncBranch(&branchPtr->trie, seq + strlen(seq) + 1);
        } else {
            return -1;
        }
    } else {

        /*
         * The end of the sequence has been reached. Finish up the job
         * and return success.
         */

        TrieTrunc(triePtr);
        return 0;
    }
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
    Branch *branchPtr;
    int     n, i;

    n = Ns_IndexCount(&triePtr->branches);

    if (n > 0) {

        /*
         * Loop over each branch and delete it
         */

        for (i = 0; i < n; i++) {
            branchPtr = Ns_IndexEl(&triePtr->branches, i);
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
TrieFind(Trie *triePtr, char *seq, int *depthPtr)
{
    Node   *nodePtr = triePtr->node;
    Branch *branchPtr;
    void   *data = NULL;
    int     ldepth = *depthPtr;

    if (nodePtr != NULL) {
        if ((*seq == '\0') && (nodePtr->dataNoInherit != NULL)) {
            data = nodePtr->dataNoInherit;
        } else {
            data = nodePtr->dataInherit;
        }
    }
    if (*seq != '\0') {

        /*
         * We have not yet reached the end of the sequence, so
         * recurse if there are any sub-branches
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
        ldepth += 1;
        if (branchPtr != NULL) {
            void *p = TrieFind(&branchPtr->trie, seq + strlen(seq) + 1, &ldepth);
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
 *      Similar to TrieFind, but will not do inheritance.
 *      If (flags & NS_OP_NOINHERIT) then data set with that flag will
 *      be returned; otherwise only data set without that flag will be
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
TrieFindExact(Trie *triePtr, char *seq, int flags)
{
    Node   *nodePtr = triePtr->node;
    Branch *branchPtr;
    void   *data = NULL;

    if (*seq != '\0') {

        /*
         * We have not reached the end of the sequence yet, so
         * we must recurse.
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
        if (branchPtr != NULL) {
            data = TrieFindExact(&branchPtr->trie, seq + strlen(seq) + 1, flags);
        }
    } else if (nodePtr != NULL) {

        /*
         * We reached the end of the sequence. Grab the data from
         * this node. If the flag specifies NOINHERIT, then return
         * the non-inheriting data, otherwise return the inheriting
         * data.
         */

        if (flags & NS_OP_NOINHERIT) {
            data = nodePtr->dataNoInherit;
        } else {
            data = nodePtr->dataInherit;
        }
    }

    return data;
}


/*
 *----------------------------------------------------------------------
 *
 * TrieDelete --
 *
 *      Delete a url, defined by a sequence, from a trie.
 *
 *      The NS_OP_NOINHERIT bit may be set in flags to use
 *      noninheriting data; NS_OP_NODELETE may be set to
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
TrieDelete(Trie *triePtr, char *seq, int flags)
{
    Node   *nodePtr = triePtr->node;
    Branch *branchPtr;
    void   *data = NULL;

    if (*seq != '\0') {

        /*
         * We have not yet reached the end of the sequence. So
         * recurse.
         */

        branchPtr = Ns_IndexFind(&triePtr->branches, seq);
        if (branchPtr != NULL) {
            data = TrieDelete(&branchPtr->trie, seq + strlen(seq) + 1, flags);
        }
    } else if (nodePtr != NULL) {

        /*
         * We've reached the end of the sequence; if a node exists for
         * this ID then delete the inheriting/noninheriting data (as
         * specified in flags) and call the delete func if requested.
         * The data will be set to null either way.
         */

        if (flags & NS_OP_NOINHERIT) {
            data = nodePtr->dataNoInherit;
            nodePtr->dataNoInherit = NULL;
            if (nodePtr->deletefuncNoInherit != NULL) {
                if (!(flags & NS_OP_NODELETE)) {
                        (*nodePtr->deletefuncNoInherit) (data);
                }
                nodePtr->deletefuncNoInherit = NULL;
            }
        } else {
            data = nodePtr->dataInherit;
            nodePtr->dataInherit = NULL;
            if (nodePtr->deletefuncInherit != NULL) {
                if (!(flags & NS_OP_NODELETE)) {
                    (*nodePtr->deletefuncInherit) (data);
                }
                nodePtr->deletefuncInherit = NULL;
            }
        }
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
 *      contain each other; 1: left contains right; -1: right contans 
 *      left.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpChannels(Channel **leftPtrPtr, Channel **rightPtrPtr)
{
    int lcontainsr, rcontainsl;

    lcontainsr = Tcl_StringMatch((*rightPtrPtr)->filter,
                                 (*leftPtrPtr)->filter);
    rcontainsl = Tcl_StringMatch((*leftPtrPtr)->filter,
                                 (*rightPtrPtr)->filter);

    if (lcontainsr && rcontainsl) {
        return 0;
    } else if (lcontainsr) {
        return 1;
    } else if (rcontainsl) {
        return -1;
    }
    return 0;
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
CmpKeyWithChannel(CONST char *key, Channel **channelPtrPtr)
{
    int lcontainsr, rcontainsl;

    lcontainsr = Tcl_StringMatch((*channelPtrPtr)->filter, key);
    rcontainsl = Tcl_StringMatch(key, (*channelPtrPtr)->filter);
    if (lcontainsr && rcontainsl) {
        return 0;
    } else if (lcontainsr) {
        return 1;
    } else if (rcontainsl) {
        return -1;
    }
    return 0;
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
 *      Same as strcmp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpChannelsAsStrings(Channel **leftPtrPtr, Channel **rightPtrPtr)
{
    return strcmp((*leftPtrPtr)->filter, (*rightPtrPtr)->filter);
}


/*
 *----------------------------------------------------------------------
 *
 * CmpKeyWithChannelAsStrings --
 *
 *      Compare a string key to a channel's filter 
 *
 * Results:
 *      Same as strcmp. 
 *
 * Side effects:
 *      None. 
 *
 *----------------------------------------------------------------------
 */

static int
CmpKeyWithChannelAsStrings(CONST char *key, Channel **channelPtrPtr)
{
    return strcmp(key, (*channelPtrPtr)->filter);
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
 *      Will initialise the junction on first access.
 *
 *----------------------------------------------------------------------
 */

Junction *
JunctionGet(NsServer *servPtr, int id)
{
    Junction *juncPtr;

    juncPtr = servPtr->urlspace[id];
    if (juncPtr == NULL) {
        juncPtr = ns_malloc(sizeof *juncPtr);
#ifndef __URLSPACE_OPTIMIZE__
        Ns_IndexInit(&juncPtr->byuse, 5,
                     (int (*) (const void *, const void *)) CmpChannels,
                     (int (*) (const void *, const void *)) CmpKeyWithChannel);
#endif
        Ns_IndexInit(&juncPtr->byname, 5,
                     (int (*) (const void *, const void *)) CmpChannelsAsStrings,
                     (int (*) (const void *, const void *)) CmpKeyWithChannelAsStrings);
        servPtr->urlspace[id] = juncPtr;
    }
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
JunctionTruncBranch(Junction *juncPtr, char *seq)
{
    Channel *channelPtr;
    int      i;
    int      n;

    /*
     * Loop over every channel in a junction and truncate the sequence in
     * each.
     */

#ifndef __URLSPACE_OPTIMIZE__
    n = Ns_IndexCount(&juncPtr->byuse);
    for (i = 0; i < n; i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
        TrieTruncBranch(&channelPtr->trie, seq);
    }
#else
    n = Ns_IndexCount(&juncPtr->byname);
    for (i = (n - 1); i >= 0; i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i);
        TrieTruncBranch(&channelPtr->trie, seq);
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
 *      Flags may be a bit-combination of NS_OP_NOINHERIT, NS_OP_NODELETE.
 *      NOINHERIT sets the data as noninheriting, so only an exact sequence
 *      will match in the future; NODELETE means that if a node already
 *      exists with this sequence/ID it will not be deleted but replaced.
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
JunctionAdd(Junction *juncPtr, char *seq, void *data, int flags,
            void (*deletefunc)(void *))
{
    Channel    *channelPtr;
    Ns_DString  dsFilter;
    char       *p;
    int         l, depth;

    depth = 0;
    Ns_DStringInit(&dsFilter);

    /*
     * Find out how deep the sequence is, and position p at the
     * beginning of the last word in the sequence.
     */

    for (p = seq; p[l = strlen(p) + 1] != '\0'; p += l) {
        depth++;
    }

    /*
     * If it's a valid sequence that has a wildcard in its last element,
     * append the whole string to dsWord, then cut off the last word from
     * p.
     * Otherwise, set dsWord to "*" because there is an implicit * wildcard
     * at the end of URLs like /foo/bar
     *
     * dsWord will eventually be used to set or find&reuse a channel filter.
     */
    
    if ((p != NULL) && (depth > 0) && (strchr(p, '*') || strchr(p, '?'))) {
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
#endif
        Ns_IndexAdd(&juncPtr->byname, channelPtr);
    }
    Ns_DStringFree(&dsFilter);

    /* 
     * Now we need to create a sequence of branches in the trie (if no
     * appropriate sequence already exists) and a node at the end of it.
     * TrieAdd will do that.
     */

    TrieAdd(&channelPtr->trie, seq, data, flags, deletefunc);
}


/*
 *----------------------------------------------------------------------
 *
 * JunctionFind --
 *
 *      Locate a node for a given sequence in a junction.
 *      As usual sequence is "method\0urltoken\0...\0\0".
 *
 *      The "fast" boolean switch makes it do strcmp instead of
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
JunctionFind(Junction *juncPtr, char *seq, int fast)
{
    Channel *channelPtr;
    char *p;
    int   l;
    int   i, n;
    void *data;
    int   depth;
    int   doit;

    n = 0;

    /*
     * After this loop, p will point at the last element in the sequence.
     * n will be the number of elements in the sequence.
     */
    
    for (p = seq; p[l = strlen(p) + 1] != '\0'; p += l) {
        n++;
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

#ifdef DEBUG
    if (depth > 0) {
        fprintf(stderr, "Checking Seq=");
        PrintSeq(seq);
        fputs("\n", stderr);
    }
#endif

    /*
     * For __URLSPACE_OPTIMIZE__
     * Basically if we use the optimize, let's reverse the order
     * by which we search because the byname is in "almost" exact
     * reverse lexicographical order.
     *
     * Loop over all the channels in the index.
     */

#ifndef __URLSPACE_OPTIMIZE__
    for (i = 0; i < l; i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    for (i = (l - 1); i >= 0; i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i);
#endif
        if (fast) {
            doit = STREQ(p, channelPtr->filter);
        } else {
            doit = Tcl_StringMatch(p, channelPtr->filter);
        }
        if (doit) {
            /*
             * We got here because this url matches the filter
             * (for example, it's *.adp).
             */

            if (data == NULL) {
                /*
                 * Nothing has been found so far. Traverse the channel
                 * and find the node; set data to that. Depth will be
                 * set to the level of the node.
                 */

                depth = 0;
                data = TrieFind(&channelPtr->trie, seq, &depth);
            } else {
                void *candidate;
                int   cdepth;

                /*
                 * Let's see if this channel has a node that also matches
                 * the sequence but is more specific (has a greater depth)
                 * that the previously found node.
                 */

                cdepth = 0;
                candidate = TrieFind(&channelPtr->trie, seq, &cdepth);
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
JunctionFindExact(Junction *juncPtr, char *seq, int flags, int fast)
{
    Channel *channelPtr;
    char *p;
    int   l;
    int   i;
    int   depth = 0;
    void *data = NULL;

    /*
     * Set p to the last element of the sequence, and
     * depth to the number of elements in the sequence.
     */

    for (p = seq; p[l = strlen(p) + 1] != '\0'; p += l) {
        depth++;
    }

    /*
     * First, loop through all the channels that have non-"*"
     * filters looking for an exact match
     */

#ifndef __URLSPACE_OPTIMIZE__
    l = Ns_IndexCount(&juncPtr->byuse);

    for (i = 0; i < l; i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    l = Ns_IndexCount(&juncPtr->byname);

    for (i = (l - 1); i >= 0; i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i);
#endif
        if (STREQ(p, channelPtr->filter)) {

            /*
             * The last element of the sequence exactly matches the
             * filter, so this is the one. Wipe out the last word and
             * return whatever node comes out of TrieFindExact.
             */

            *p = '\0';
            data = TrieFindExact(&channelPtr->trie, seq, flags);
            goto done;
        }
    }

    /*
     * Now go to the channel with the "*" filter and look there for 
     * an exact match:
     */

#ifndef __URLSPACE_OPTIMIZE__
    for (i = 0; i < l; i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    for (i = (l - 1); i >= 0; i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i);
#endif
        if (STREQ("*", channelPtr->filter)) {
            data = TrieFindExact(&channelPtr->trie, seq, flags);
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
JunctionDeleteNode(Junction *juncPtr, char *seq, int flags)
{
    Channel *channelPtr;
    char *p;
    int   l;
    int   i;
    int   depth = 0;
    void *data = NULL;

    /*
     * Set p to the last element of the sequence, and
     * depth to the number of elements in the sequence.
     */

    for (p = seq; p[l = strlen(p) + 1] != '\0'; p += l) {
        depth++;
    }

#ifndef __URLSPACE_OPTIMIZE__
    l = Ns_IndexCount(&juncPtr->byuse);
    for (i = 0; (i < l) && (data == NULL); i++) {
        channelPtr = Ns_IndexEl(&juncPtr->byuse, i);
#else
    l = Ns_IndexCount(&juncPtr->byname);
    for (i = (l - 1); (i >= 0) && (data == NULL); i--) {
        channelPtr = Ns_IndexEl(&juncPtr->byname, i);
#endif
        if (depth == 2 && STREQ(p, channelPtr->filter)) {

            /*
             * This filter exactly matches the last element of the
             * sequence, so get the node and delete it. (This is
             * server-specific data because depth is 2).
             */

            *p = '\0';
            data = TrieFindExact(&channelPtr->trie, seq, flags);
            if (data != NULL) {
                TrieDelete(&channelPtr->trie, seq, flags);
            }
        } else if (Tcl_StringMatch(p, channelPtr->filter)) {

            /*
             * The filter matches, so get the node and delete it.
             */

            data = TrieFindExact(&channelPtr->trie, seq, flags);
            if (data != NULL) {
                TrieDelete(&channelPtr->trie, seq, flags);
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
MkSeq(Ns_DString *dsPtr, CONST char *method, CONST char *url)
{
    CONST char *p;
    int         done, l;

    Ns_DStringNAppend(dsPtr, method, (int)(strlen(method) + 1));

    /*
     * Loop over each directory in the URL and turn the slashes
     * into nulls.
     */

    done = 0;
    while (!done && *url != '\0') {
        if (*url != '/') {
            p = strchr(url, '/');
            if (p != NULL) {
                l = p - url;
            } else {
                l = strlen(url);
                done = 1;
            }

            Ns_DStringNAppend(dsPtr, url, l++);
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
PrintSeq(CONST char *seq)
{
    CONST char *p;

    for (p = seq; *p != '\0'; p += strlen(p) + 1) {
        if (p != seq) {
            fputs(", ", stderr);
        }
        fputs(p, stderr);
    }
}
#endif
