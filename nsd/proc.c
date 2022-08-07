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
 * proc.c --
 *
 *      Support for describing procs and their arguments (thread routines,
 *      callbacks, scheduled procs, etc.).
 */

#include "nsd.h"

/*
 * The following struct maintains callback and description for
 * Ns_GetProcInfo.
 */

typedef struct Info {
    Ns_ArgProc  *proc;
    const char  *desc;
} Info;

/*
 * Static functions defined in this file.
 */

static Ns_ArgProc ServerArgProc;
static void AppendAddr(Tcl_DString *dsPtr, const char *prefix, const void *addr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Tcl_HashKeyProc         FuncptrKey;
static Tcl_CompareHashKeysProc CompareFuncptrKeys;
static Tcl_AllocHashEntryProc  AllocFuncptrEntry;
static Tcl_FreeHashEntryProc   FreeFuncptrEntry;

/*
 * Static variables defined in this file.
 */

static const Tcl_HashKeyType funPtrHashKeyType = {
  1,                  /* version         */
  0,                  /* flags           */
  FuncptrKey,         /* hashKeyProc     */
  CompareFuncptrKeys, /* compareKeysProc */
  AllocFuncptrEntry,  /* allocEntryProc  */
  FreeFuncptrEntry    /* freeEntryProc   */
};

typedef struct funcptrEntry_t {
  ns_funcptr_t funcptr;
} funcptrEntry_t;

static Tcl_HashTable infoHashTable;

static const struct proc {
    ns_funcptr_t  procAddr;
    const char   *desc;
    Ns_ArgProc   *argProc;
} procs[] = {
    { (ns_funcptr_t)NsTclThread,          "ns:tclthread",        NsTclThreadArgProc},
    { (ns_funcptr_t)Ns_TclCallbackProc,   "ns:tclcallback",      Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsTclConnLocation,    "ns:tclconnlocation",  Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsTclSchedProc,       "ns:tclschedproc",     Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsTclServerRoot,      "ns:tclserverroot",    Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsTclSockProc,        "ns:tclsockcallback",  NsTclSockArgProc},
    { (ns_funcptr_t)NsConnThread,         "ns:connthread",       NsConnArgProc},
    { (ns_funcptr_t)NsTclFilterProc,      "ns:tclfilter",        Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsShortcutFilterProc, "ns:shortcutfilter",   NULL},
    { (ns_funcptr_t)NsTclRequestProc,     "ns:tclrequest",       Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsAdpPageProc,        "ns:adppage",          NsAdpPageArgProc},
    { (ns_funcptr_t)Ns_FastPathProc,      "ns:fastget",          NULL},
    { (ns_funcptr_t)NsTclTraceProc,       "ns:tcltrace",         Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsTclUrl2FileProc,    "ns:tclurl2file",      Ns_TclCallbackArgProc},
    { (ns_funcptr_t)NsMountUrl2FileProc,  "ns:mounturl2file",    NsMountUrl2FileArgProc},
    { (ns_funcptr_t)Ns_FastUrl2FileProc,  "ns:fasturl2file",     ServerArgProc},
    {NULL, NULL, NULL}
};


/*
 *----------------------------------------------------------------------
 *
 * AllocFuncptrEntry --
 *
 *      Allocate enough space for a Tcl_HashEntry including the payload. The
 *      function pointer is assigned to key.oneWordValue via memcpy().
 *
 * Results:
 *      Return a memory block casted to Tcl_HashEntry*.
 *
 * Side effects:
 *      Memory allocation.
 *----------------------------------------------------------------------
 */

static Tcl_HashEntry *
AllocFuncptrEntry(Tcl_HashTable *UNUSED(tablePtr), void *keyPtr) {
  Tcl_HashEntry  *hPtr;
  ns_funcptr_t    value = ((funcptrEntry_t *)keyPtr)->funcptr;

  /*
   * The size of the function pointer might be larger than the data pointer.
   * since we store the value starting with the oneWordValue, we have to
   * allocate on some architectures a more than the Tcl_HashEntry, namely the
   * differences of the sizes of these pointer types.
   */
  hPtr = (Tcl_HashEntry *)ns_malloc(sizeof(Tcl_HashEntry) +
                                    MAX(sizeof(ns_funcptr_t), sizeof(char*)) - sizeof(char*));
  hPtr->clientData = NULL;

  memcpy(&hPtr->key.oneWordValue, &value, sizeof(value));

  return hPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeFuncptrEntry --
 *
 *      Free an entry in the funcptr hash table. The inverse operation of
 *      AllocFuncptrEntry().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Free memory.
 *----------------------------------------------------------------------
 */
static void
FreeFuncptrEntry(Tcl_HashEntry *hPtr)
{
    ns_free(hPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * FuncptrKey --
 *
 *      Compute an unsigned int hash value from a function pointer.
 *
 * Results:
 *      Returns the computed hash.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static unsigned int
FuncptrKey(Tcl_HashTable *UNUSED(tablePtr), void *keyPtr)
{
  /*
   * Simply return the value part of the funcptrEntry as hash value.
   */
  return PTR2UINT(((funcptrEntry_t *)keyPtr)->funcptr);
}

/*
 *----------------------------------------------------------------------
 *
 * CompareFuncptrKeys --
 *
 *      Compare two function pointers.
 *
 * Results:
 *      The return value is 0 if they are different and 1 if they are the
 *      same.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CompareFuncptrKeys(void *keyPtr, Tcl_HashEntry *hPtr)
{
  ns_funcptr_t funcptr;

  memcpy(&funcptr, &hPtr->key.oneWordValue, sizeof(ns_funcptr_t));

  return ((funcptrEntry_t *)keyPtr)->funcptr == funcptr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitProcInfo --
 *
 *      Initialize the proc info API and default compiled-in callbacks.
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
NsInitProcInfo(void)
{
    const struct proc *procPtr;

    Tcl_InitCustomHashTable(&infoHashTable, TCL_CUSTOM_PTR_KEYS, &funPtrHashKeyType);
    procPtr = procs;
    while (procPtr->procAddr != NULL) {
        Ns_RegisterProcInfo(procPtr->procAddr, procPtr->desc,
                            procPtr->argProc);
        ++procPtr;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterProcInfo --
 *
 *      Register a proc description and a callback to describe the
 *      arguments e.g., a thread start arg.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Given argProc will be invoked for given procAddr by
 *      Ns_GetProcInfo.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RegisterProcInfo(ns_funcptr_t procAddr, const char *desc, Ns_ArgProc *argProc)
{
    Tcl_HashEntry *hPtr;
    Info          *infoPtr;
    int            isNew;
    funcptrEntry_t entry;

    NS_NONNULL_ASSERT(procAddr != NULL);
    NS_NONNULL_ASSERT(desc != NULL);

    entry.funcptr = procAddr;
    hPtr = Tcl_CreateHashEntry(&infoHashTable, (const char *)&entry, &isNew);

    if (isNew == 0) {
        infoPtr = Tcl_GetHashValue(hPtr);
    } else {
        infoPtr = ns_malloc(sizeof(Info));
        Tcl_SetHashValue(hPtr, infoPtr);
    }
    infoPtr->desc = desc;
    infoPtr->proc = argProc;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetProcInfo --
 *
 *      Format a string of information for the given proc
 *      and arg, invoking the argProc callback if it exists.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      String will be appended to given dsPtr.
 *
 *----------------------------------------------------------------------
 */

void
Ns_GetProcInfo(Tcl_DString *dsPtr, ns_funcptr_t procAddr, const void *arg)
{
    const Tcl_HashEntry  *hPtr;
    const Info           *infoPtr;
    static const Info     nullInfo = {NULL, NULL};
    funcptrEntry_t        entry;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    entry.funcptr = procAddr;
    hPtr = Tcl_FindHashEntry(&infoHashTable, (const char *)&entry);

    if (hPtr != NULL) {
        infoPtr = Tcl_GetHashValue(hPtr);
    } else {
        infoPtr = &nullInfo;
    }
    /* Ns_Log(Notice, "Ns_GetProcInfo: infoPtr->desc %p", infoPtr->desc);*/
    if (infoPtr->desc != NULL) {
        Tcl_DStringAppendElement(dsPtr, infoPtr->desc);
    } else {
        /*
         * The following is a rather crude approach obtaining a hex print
         * string from a function pointer. For our purposes, this should be
         * good enough.
         */
        union {
            ns_funcptr_t funcptr;
            void        *ptr;
        } data;

        data.funcptr = procAddr;
        AppendAddr(dsPtr, "p", data.ptr);
    }
    /*Ns_Log(Notice, "Ns_GetProcInfo: infoPtr->proc %p", infoPtr->proc);*/
    if (infoPtr->proc != NULL) {
        (*infoPtr->proc)(dsPtr, arg);
    } else {
        AppendAddr(dsPtr, "a", arg);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StringArgProc --
 *
 *      Info callback for procs which take a C string arg.
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
Ns_StringArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const char *str = arg;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Tcl_DStringAppendElement(dsPtr, (str != NULL) ? str : NS_EMPTY_STRING);
}


/*
 *----------------------------------------------------------------------
 *
 * ServerArgProc --
 *
 *      Info callback for procs which take an NsServer arg.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
ServerArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const NsServer *servPtr = arg;

    Tcl_DStringAppendElement(dsPtr, (servPtr != NULL) ? servPtr->server : NS_EMPTY_STRING);
}


/*
 *----------------------------------------------------------------------
 *
 * AppendAddr --
 *
 *      Format a simple string with the given address.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      String will be appended to given dsPtr.
 *
 *----------------------------------------------------------------------
 */

static void
AppendAddr(Tcl_DString *dsPtr, const char *prefix, const void *addr)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(prefix != NULL);

    Ns_DStringPrintf(dsPtr, " %s:%p", prefix, addr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
