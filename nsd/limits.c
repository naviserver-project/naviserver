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
 * limits.c --
 *
 *      Routines to manage request resource limits.
 */


#include "nsd.h"


/*
 * Static functions defined in this file.
 */

static Ns_ObjvProc ObjvLimits;
static NsLimits *FindLimits(const char *limits, int create)
    NS_GNUC_NONNULL(1);

static void LimitsResult(Tcl_Interp *interp, const NsLimits *limitsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * Static variables defined in this file.
 */

static int            limid = 0;
static NsLimits      *defLimitsPtr;     /* Default limits if none registered. */
static Tcl_HashTable  limtable;         /* Process-wide hash of limits. */
static Ns_Mutex       lock = NULL;      /* Lock for limtable and urlspecific data. */



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

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Ns_MutexLock(&lock);
    limitsPtr = Ns_UrlSpecificGet((Ns_Server*)servPtr, method, url, limid, 0u,
                                  NS_URLSPACE_DEFAULT, NULL, NULL, NULL);
    Ns_MutexUnlock(&lock);

    return ((limitsPtr != NULL) ? limitsPtr : defLimitsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetLimitsObjCmd --
 *
 *      Implements "ns_limits_get". Get the named limits.
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
NsTclGetLimitsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    NsLimits    *limitsPtr;
    Ns_ObjvSpec  args[] = {
        {"limits", ObjvLimits, &limitsPtr, INT2PTR(NS_FALSE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        LimitsResult(interp, limitsPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclListLimitsObjCmd --
 *
 *      Implements "ns_limits_list". Returns the names of all limits, or only
 *      those matching a pattern.
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
NsTclListLimitsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?/pattern/?");
        result = TCL_ERROR;
    } else {
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;
        const char          *pattern = (objc == 2 ? Tcl_GetString(objv[1]) : NULL);
        Tcl_Obj             *listObj = Tcl_NewListObj(0, NULL);

        Ns_MutexLock(&lock);
        hPtr = Tcl_FirstHashEntry(&limtable, &search);
        while (hPtr != NULL) {
            const char *limits = Tcl_GetHashKey(&limtable, hPtr);

            if (pattern == NULL || Tcl_StringMatch(limits, pattern) != 0) {
                Tcl_ListObjAppendElement(interp, listObj,
                                         Tcl_NewStringObj(limits, TCL_INDEX_NONE));
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&lock);
        Tcl_SetObjResult(interp, listObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSetLimitsObjCmd --
 *
 *      Implements "ns_limits_set". Adjust the values of the named limits, or
 *      create a new named limits with default values.
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
NsTclSetLimitsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    NsLimits         *limitsPtr;
    int               maxrun = -1, maxwait = -1, maxupload = -1, timeout = -1, result = TCL_OK;
    Ns_ObjvValueRange range = {0, INT_MAX};
    Ns_ObjvSpec opts[] = {
        {"-maxrun",    Ns_ObjvInt,   &maxrun,    &range},
        {"-maxwait",   Ns_ObjvInt,   &maxwait,   &range},
        {"-maxupload", Ns_ObjvInt,   &maxupload, &range},
        {"-timeout",   Ns_ObjvInt,   &timeout,   &range},
        {"--",         Ns_ObjvBreak, NULL,       &range},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"limits", ObjvLimits, &limitsPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
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
            limitsPtr->timeout = (long)timeout;
        }
        LimitsResult(interp, limitsPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterLimitsObjCmd --
 *
 *      Implements "ns_limits_register". Register the named limits to a
 *      method/URL.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May override already registered limits.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterLimitsObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    NsLimits       *limitsPtr;
    char           *method, *url;
    int             noinherit = 0, result = TCL_OK;
    Ns_ObjvSpec     opts[] = {
        {"-noinherit", Ns_ObjvBool,   &noinherit, INT2PTR(NS_TRUE)},
        {"-server",    Ns_ObjvServer, &servPtr,   NULL},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"limits", ObjvLimits,    &limitsPtr, INT2PTR(NS_FALSE)},
        {"method", Ns_ObjvString, &method,    NULL},
        {"url",    Ns_ObjvString, &url,       NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int flags = 0u;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }
        Ns_MutexLock(&lock);
        Ns_UrlSpecificSet(servPtr->server, method, url,
                          limid, limitsPtr, flags, NULL);
        Ns_MutexUnlock(&lock);
    }

    return result;
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
    Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(limits != NULL);

    Ns_MutexLock(&lock);
    if (create == 0) {
        hPtr = Tcl_FindHashEntry(&limtable, limits);
    } else {
        int isNew = 0;

        hPtr = Tcl_CreateHashEntry(&limtable, limits, &isNew);
        if (isNew != 0) {
            NsLimits *limitsPtr;

            limitsPtr = ns_calloc(1u, sizeof(NsLimits));
            limitsPtr->name = Tcl_GetHashKey(&limtable, hPtr);
            Ns_MutexInit(&limitsPtr->lock);
            Ns_MutexSetName2(&limitsPtr->lock, "ns:limits", limits);
            limitsPtr->maxrun = limitsPtr->maxwait = 100u;
            limitsPtr->maxupload = 10u * 1024u * 1000u; /* NB: 10meg limit. */
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
ObjvLimits(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    NsLimits         **limitsPtrPtr = spec->dest;
    int                create = (spec->arg != NULL) ? 1 : 0, result = TCL_OK;

    if (*objcPtr < 1) {
        result = TCL_ERROR;

    } else {
        static const char *const limitsType = "ns:limits";

        if (Ns_TclGetOpaqueFromObj(objv[0], limitsType, (void **) limitsPtrPtr)
            != TCL_OK) {
            const char *limits = Tcl_GetString(objv[0]);

            *limitsPtrPtr = FindLimits(limits, create);
            if (*limitsPtrPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such limits: %s", limits);
                result = TCL_ERROR;
            } else {
                Ns_TclSetOpaqueObj(objv[0], limitsType, *limitsPtrPtr);
            }
        }
        if (result == TCL_OK) {
            *objcPtr -= 1;
        }
    }
    return result;
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
    Tcl_DString ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(limitsPtr != NULL);

    Tcl_DStringInit(&ds);
    Ns_DStringPrintf(&ds, "nrunning %u nwaiting %u"
                     " ntimeout %u ndropped %u noverflow %u"
                     " maxrun %u maxwait %u"
                     " maxupload %lu timeout %ld",
                     limitsPtr->state.nrunning, limitsPtr->state.nwaiting,
                     limitsPtr->stats.ntimeout, limitsPtr->stats.ndropped,
                         limitsPtr->stats.noverflow,
                     limitsPtr->maxrun, limitsPtr->maxwait,
                     (unsigned long) limitsPtr->maxupload, limitsPtr->timeout);
    Tcl_DStringResult(interp, &ds);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
