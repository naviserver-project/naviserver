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
static void AppendAddr(Tcl_DString *dsPtr, char *prefix, const void *addr);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable infoHashTable;

static struct proc {
    Ns_Callback *procAddr;
    const char  *desc;
    Ns_ArgProc  *argProc;
} procs[] = {
    {                NsTclThread,          "ns:tclthread",        NsTclThreadArgProc},
    {                Ns_TclCallbackProc,   "ns:tclcallback",      Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsTclConnLocation,    "ns:tclconnlocation",  Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsTclSchedProc,       "ns:tclschedproc",     Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsTclServerRoot,      "ns:tclserverroot",    Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsTclSockProc,        "ns:tclsockcallback",  NsTclSockArgProc},
    {                NsConnThread,         "ns:connthread",       NsConnArgProc},
    { (Ns_Callback *)NsTclFilterProc,      "ns:tclfilter",        Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsShortcutFilterProc, "ns:shortcutfilter",   NULL},
    { (Ns_Callback *)NsTclRequestProc,     "ns:tclrequest",       Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsAdpPageProc,        "ns:adppage",          NsAdpPageArgProc},
    { (Ns_Callback *)Ns_FastPathProc,      "ns:fastget",          NULL},
    { (Ns_Callback *)NsTclTraceProc,       "ns:tcltrace",         Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsTclUrl2FileProc,    "ns:tclurl2file",      Ns_TclCallbackArgProc},
    { (Ns_Callback *)NsMountUrl2FileProc,  "ns:mounturl2file",    NsMountUrl2FileArgProc},
    { (Ns_Callback *)Ns_FastUrl2FileProc,  "ns:fasturl2file",     ServerArgProc},
    {NULL, NULL, NULL}
};


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
    struct proc *procPtr;

    Tcl_InitHashTable(&infoHashTable, TCL_ONE_WORD_KEYS);
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
Ns_RegisterProcInfo(Ns_Callback procAddr, const char *desc, Ns_ArgProc *argProc)
{
    Tcl_HashEntry *hPtr;
    Info          *infoPtr;
    int            isNew;

    hPtr = Tcl_CreateHashEntry(&infoHashTable, (char *)procAddr, &isNew);
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
Ns_GetProcInfo(Tcl_DString *dsPtr, Ns_Callback procAddr, const void *arg)
{
    Tcl_HashEntry          *hPtr;
    Info                   *infoPtr;
    static Info nullInfo =  {NULL, NULL};

    hPtr = Tcl_FindHashEntry(&infoHashTable, (char *) procAddr);
    if (hPtr != NULL) {
        infoPtr = Tcl_GetHashValue(hPtr);
    } else {
        infoPtr = &nullInfo;
    }
    if (infoPtr->desc != NULL) {
        Tcl_DStringAppendElement(dsPtr, infoPtr->desc);
    } else {
        AppendAddr(dsPtr, "p", (void *)procAddr);
    }
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
 *      Info callback for procs which take a cstring arg.
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
Ns_StringArgProc(Tcl_DString *dsPtr, void *arg)
{
    char *str = arg;

    Tcl_DStringAppendElement(dsPtr, (str != 0) ? str : "");
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

    Tcl_DStringAppendElement(dsPtr, (servPtr != NULL) ? servPtr->server : "");
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
AppendAddr(Tcl_DString *dsPtr, char *prefix, const void *addr)
{
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
