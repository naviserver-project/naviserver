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
 * nsproxy.h --
 *
 *      Definitions for simple worker process proxies.
 */

#ifndef NSPROXY_H
#define NSPROXY_H

#include "ns.h"

#ifdef NSPROXY_EXPORTS
#undef  NS_EXTERN
#define NS_EXTERN extern NS_EXPORT
#endif

/*
 * The following structure is allocated per-interp
 * to manage per-interp state of the module. This
 * is used from both nsproxymod.c and nsproxylib.c
 */

typedef struct InterpData {
    char *server;
    char *module;
    Tcl_HashTable ids;
    Tcl_HashTable cnts;
} InterpData;

#define ASSOC_DATA "nsproxy:data"


NS_EXTERN void Nsproxy_LibInit (void);
NS_EXTERN int Nsproxy_Init(Tcl_Interp *interp) NS_GNUC_NONNULL(1);

NS_EXTERN int Ns_ProxyMain(int argc, char *const*argv, Tcl_AppInitProc *init);
NS_EXTERN int Ns_ProxyTclInit(Tcl_Interp *interp);

NS_EXTERN Ns_TclTraceProc  Ns_ProxyCleanup;

/*
 * Small proxy API so C-level code can also
 * take advantage of proxy communication
 */

typedef void* PROXY;

NS_EXTERN int  Ns_ProxyGet(Tcl_Interp *interp, const char *poolName, PROXY *handlePtr,
                           Ns_Time *timePtr);
NS_EXTERN int  Ns_ProxyEval(Tcl_Interp *interp, PROXY handle, const char *script,
                            const Ns_Time *timeoutPtr);
NS_EXTERN void Ns_ProxyPut(PROXY handle);

#endif /* NSPROXY_H */


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
