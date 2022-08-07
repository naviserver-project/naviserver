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
 * nsproxymod.c --
 *
 *      Loadable module for NaviServer to add the ns_proxy command
 *      and cleanup trace.
 */

#include "nsproxy.h"

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

typedef struct {
    char *server;
    char *module;
} SrvMod;

/*
 * Static functions defined in this file.
 */

static Ns_TclTraceProc InitInterp;
NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;



/*
 *----------------------------------------------------------------------
 *
 * Nsproxy_Init --
 *
 *      Tcl load entry point.
 *
 * Results:
 *      See Ns_ProxyTclInit.
 *
 * Side effects:
 *      See Ns_ProxyTclInit.
 *
 *----------------------------------------------------------------------
 */

int
Nsproxy_Init(Tcl_Interp *interp)
{
    Nsproxy_LibInit();
    return Ns_ProxyTclInit(interp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      NaviServer module initialization routine.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    SrvMod       *smPtr;
    static bool   initialized = NS_FALSE;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(module != NULL);

    if (!initialized) {
        initialized = NS_TRUE;
        Nsproxy_LibInit();
        Ns_RegisterProcInfo((ns_funcptr_t)InitInterp, "nsproxy:initinterp", NULL);
        Ns_RegisterProcInfo((ns_funcptr_t)Ns_ProxyCleanup, "nsproxy:cleanup", NULL);
    }

    smPtr = ns_malloc(sizeof(SrvMod));
    smPtr->server = ns_strdup(server);
    smPtr->module = ns_strdup(module);

    result = Ns_TclRegisterTrace(server, InitInterp, smPtr, NS_TCL_TRACE_CREATE);
    if (result == NS_OK) {
      result = Ns_TclRegisterTrace(server, Ns_ProxyCleanup, NULL, NS_TCL_TRACE_DEALLOCATE);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * InitInterp --
 *
 *      Initialize interpreter
 *
 * Results:
 *      Ns_ReturnCode
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
InitInterp(Tcl_Interp *interp, const void *arg)
{
    const SrvMod *smPtr = arg;
    Ns_ReturnCode status = NS_OK;

    if (Ns_ProxyTclInit(interp) == TCL_OK) {
        InterpData *idataPtr = Tcl_GetAssocData(interp, ASSOC_DATA, NULL);

        idataPtr->server = smPtr->server;
        idataPtr->module = smPtr->module;
    }

    return status;
}
