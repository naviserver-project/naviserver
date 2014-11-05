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
 * limits.c --
 *
 *      Routines to manage request resource limits.
 */


#include "nsd.h"

#define CREATE   NS_TRUE
#define NOCREATE NS_FALSE


/*
 * Static functions defined in this file.
 */

static Ns_ObjvProc ObjvLimits;
static NsLimits *FindLimits(const char *limits, int create);
static void LimitsResult(Tcl_Interp *interp, const NsLimits *limitsPtr);


/*
 * Static variables defined in this file.
 */

static int            limid = 0;
static NsLimits      *defLimitsPtr;     /* Default limits if none registered. */
static Tcl_HashTable  limtable;         /* Process-wide hash of limits. */
static Ns_Mutex       lock = 0;         /* Lock for limtable and urlspecific data. */



/*
 *----------------------------------------------------------------------
 *
 * NsInitLimits --
 *
 *      Initialize request limits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will create the default limits.
 *
 *----------------------------------------------------------------------
 */

void
NsInitLimits(void)
{
    limid = Ns_UrlSpecificAlloc();
    Tcl_InitHashTable(&limtable, TCL_STRING_KEYS);
    Ns_MutexSetName(&lock, "ns:limits");
    defLimitsPtr = FindLimits("default", 1);
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetRequestLimits --
 *
 *      Return the limits structure for a given request.
 *
 * Results:
 *      Pointer to limits.
 *
 * Side effects:
 *      May return the default limits if no more specific limits
 *      have been registered.
 *
 *----------------------------------------------------------------------
 */

NsLimits *
NsGetRequestLimits(NsServer *servPtr, const char *method, const char *url)
{
    NsLimits *limitsPtr;

    Ns_MutexLock(&lock);
    limitsPtr = NsUrlSpecificGet(servPtr, method, url, limid, 0);
    Ns_MutexUnlock(&lock);

    return ((limitsPtr != NULL) ? limitsPtr : defLimitsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetLimitsObjCmd --
 *
 *      Get the named limits.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclGetLimitsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsLimits *limitsPtr;

    Ns_ObjvSpec args[] = {
        {"limits", ObjvLimits, &limitsPtr, INT2PTR(NOCREATE)},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    LimitsResult(interp, limitsPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclListLimitsObjCmd --
 *
 *      Return the names of all limits, or only those matching a pattern.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclListLimitsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    char           *pattern;

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?pattern?");
        return TCL_ERROR;
    }
    pattern = (objc == 2 ? Tcl_GetString(objv[1]) : NULL);
    Ns_MutexLock(&lock);
    hPtr = Tcl_FirstHashEntry(&limtable, &search);
    while (hPtr != NULL) {
        char *limits = Tcl_GetHashKey(&limtable, hPtr);

        if (pattern == NULL || Tcl_StringMatch(limits, pattern) != 0) {
            Tcl_AppendElement(interp, limits);
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&lock);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSetLimitsObjCmd --
 *
 *      Adjust the values of the named limits, or create a new named
 *      limits with default values.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      New limits may be created if not already existing.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSetLimitsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsLimits *limitsPtr;
    int       maxrun = -1, maxwait = -1, maxupload = -1, timeout = -1;

    Ns_ObjvSpec opts[] = {
        {"-maxrun",    Ns_ObjvInt,   &maxrun,    NULL},
        {"-maxwait",   Ns_ObjvInt,   &maxwait,   NULL},
        {"-maxupload", Ns_ObjvInt,   &maxupload, NULL},
        {"-timeout",   Ns_ObjvInt,   &timeout,   NULL},
        {"--",         Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"limits", ObjvLimits, &limitsPtr, INT2PTR(CREATE)},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (maxrun > -1) {
	limitsPtr->maxrun = (unsigned int)maxrun;
    }
    if (maxwait > -1) {
        limitsPtr->maxwait = (unsigned int)maxwait;
    }
    if (maxupload > -1) {
	limitsPtr->maxupload = (size_t)maxupload;
    }
    if (timeout > -1) {
        limitsPtr->timeout = timeout;
    }
    LimitsResult(interp, limitsPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterLimitsObjCmd --
 *
 *      Register the named limits to a method/URL.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May overide already registred limits.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterLimitsObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp    *itPtr = clientData;
    NsLimits    *limitsPtr;
    char        *method, *url, *server = itPtr->servPtr->server;
    int          noinherit = 0;
    unsigned int flags = 0U;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,   &noinherit, INT2PTR(1)},
        {"-server",    Ns_ObjvString, &server,    NULL},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"limits", ObjvLimits,    &limitsPtr, INT2PTR(NOCREATE)},
        {"method", Ns_ObjvString, &method,    NULL},
        {"url",    Ns_ObjvString, &url,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit != 0) {flags |= NS_OP_NOINHERIT;}
    Ns_MutexLock(&lock);
    Ns_UrlSpecificSet(server, method, url,
                      limid, limitsPtr, flags, NULL);
    Ns_MutexUnlock(&lock);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * FindLimits --
 *
 *      Return the limits by name.
 *
 * Results:
 *      Pointer to NsLimits or NULL if no such limits and create is zero.
 *
 * Side effects:
 *      If create is not zero, will create new default limits.
 *
 *----------------------------------------------------------------------
 */

static NsLimits *
FindLimits(const char *limits, int create)
{
    NsLimits      *limitsPtr;
    Tcl_HashEntry *hPtr;
    int            isNew;

    Ns_MutexLock(&lock);
    if (create == 0) {
        hPtr = Tcl_FindHashEntry(&limtable, limits);
    } else {
        hPtr = Tcl_CreateHashEntry(&limtable, limits, &isNew);
        if (isNew != 0) {
            limitsPtr = ns_calloc(1U, sizeof(NsLimits));
            limitsPtr->name = Tcl_GetHashKey(&limtable, hPtr);
            Ns_MutexInit(&limitsPtr->lock);
            Ns_MutexSetName2(&limitsPtr->lock, "ns:limits", limits);
            limitsPtr->maxrun = limitsPtr->maxwait = 100U;
            limitsPtr->maxupload = 10U * 1024U * 1000U; /* NB: 10meg limit. */
            limitsPtr->timeout = 60;
            Tcl_SetHashValue(hPtr, limitsPtr);
        }
    }
    Ns_MutexUnlock(&lock);

    return ((hPtr != NULL) ? Tcl_GetHashValue(hPtr) : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * ObjvLimits --
 *
 *      Objv proc to lookup limits by name.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Will update limitsPtrPtr with pointer to NsLimits or leave
 *      an error message in given interp if no limits found and
 *      create is zero. 
 *
 *----------------------------------------------------------------------
 */

static int
ObjvLimits(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
           Tcl_Obj *CONST* objv)
{
    NsLimits          **limitsPtrPtr = spec->dest;
    int                 create = (spec->arg != NULL) ? 1 : 0;
    static const char  *limitsType = "ns:limits";

    if (*objcPtr < 1) {
        return TCL_ERROR;
    }
    if (Ns_TclGetOpaqueFromObj(objv[0], limitsType, (void **) limitsPtrPtr)
        != TCL_OK) {
        char *limits = Tcl_GetString(objv[0]);
        *limitsPtrPtr = FindLimits(limits, create);
        if (*limitsPtrPtr == NULL) {
            Tcl_AppendResult(interp, "no such limits: ", limits, NULL);
            return TCL_ERROR;
        }
        Ns_TclSetOpaqueObj(objv[0], limitsType, *limitsPtrPtr);
    }
    *objcPtr -= 1;

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LimitsResult --
 *
 *      Return a list of info about given limits.
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
LimitsResult(Tcl_Interp *interp, const NsLimits *limitsPtr)
{
    Ns_DString ds;

    Ns_DStringInit(&ds);
    Ns_DStringPrintf(&ds, "nrunning %u nwaiting %u"
                     " ntimeout %u ndropped %u noverflow %u"
                     " maxrun %u maxwait %u"
                     " maxupload %lu timeout %d",
                     limitsPtr->state.nrunning, limitsPtr->state.nwaiting,
                     limitsPtr->stats.ntimeout, limitsPtr->stats.ndropped,
                         limitsPtr->stats.noverflow,
                     limitsPtr->maxrun, limitsPtr->maxwait,
                     (unsigned long) limitsPtr->maxupload, limitsPtr->timeout);
    Tcl_DStringResult(interp, &ds);
}
