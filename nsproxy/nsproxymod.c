/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
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
 * nsproxymod.c --
 *
 *      Loadable module for Naviserver to add the ns_proxy command
 *      and cleanup trace.
 */

#include "nsproxy.h"

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
 *      NaviServer module initialisation routine.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT int
Ns_ModuleInit(const char *server, const char *module)
{
    SrvMod *smPtr;
    static  int once = 0;
    int     result;

    NS_NONNULL_ASSERT(module != NULL);

    if (once == 0) {
        once = 1;
        Nsproxy_LibInit();
        Ns_RegisterProcInfo((Ns_Callback *)InitInterp, "nsproxy:initinterp", NULL);
        Ns_RegisterProcInfo((Ns_Callback *)Ns_ProxyCleanup, "nsproxy:cleanup", NULL);
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

static int
InitInterp(Tcl_Interp *interp, const void *arg)
{
    const SrvMod *smPtr = arg;
    int           status;

    status = Ns_ProxyTclInit(interp);

    if (status == TCL_OK) {
        InterpData *idataPtr = Tcl_GetAssocData(interp, ASSOC_DATA, NULL);
        idataPtr->server = smPtr->server;
        idataPtr->module = smPtr->module;
    }

    return status;
}
