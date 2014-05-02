/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozzila.org/.
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
 * nsproxy.h --
 *
 *      Definitions for simple slave-process proxies.
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

NS_EXTERN int Ns_ProxyMain (int argc, char **argv, Tcl_AppInitProc *proc);
NS_EXTERN int Ns_ProxyTclInit (Tcl_Interp *interp);

NS_EXTERN Ns_TclTraceProc  Ns_ProxyCleanup;

/*
 * Small proxy API so C-level code can also
 * take advantage of proxy communication
 */

typedef void* PROXY;

NS_EXTERN int  Ns_ProxyGet  (Tcl_Interp *interp, char *pool, PROXY *handlePtr,
                             int ms);
NS_EXTERN int  Ns_ProxyEval (Tcl_Interp *interp, PROXY handle, char *script,
                             int ms);
NS_EXTERN void Ns_ProxyPut  (PROXY handle);

#endif /* NSPROXY_H */
