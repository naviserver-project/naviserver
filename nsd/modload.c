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
 * modload.c --
 *
 *      Load module files into the server and initialize them. 
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

/*
 * The following structure is used for static module loading.
 */

typedef struct Module {
    struct Module     *nextPtr;
    char              *name;
    Ns_ModuleInitProc *proc;
} Module;

/*
 * Static variables defined in this file.
 */

static Module *firstPtr;           /* List of static modules to be inited. */


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterModule --
 *
 *      Register a static module.  This routine can only be called from
 *      a Ns_ServerInitProc passed to Ns_Main or within the Ns_ModuleInit
 *      proc of a loadable module.  It registers a module callback for
 *      for the currently initializing server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Proc will be called after dynamic modules are loaded.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RegisterModule(CONST char *name, Ns_ModuleInitProc *proc)
{
    Module *modPtr, **nextPtrPtr;

    modPtr = ns_malloc(sizeof(Module));
    modPtr->name = ns_strcopy(name);
    modPtr->proc = proc;
    modPtr->nextPtr = NULL;
    nextPtrPtr = &firstPtr;
    while (*nextPtrPtr != NULL) {
        nextPtrPtr = &((*nextPtrPtr)->nextPtr); 
    }
    *nextPtrPtr = modPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleLoad --
 *
 *      Load a module and initialize it.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      The result code from modules w/o the version symbol is ignored
 *
 *----------------------------------------------------------------------
 */

int
Ns_ModuleLoad(CONST char *server, CONST char *module, CONST char *file,
              CONST char *init)
{
    Tcl_PackageInitProc  *tclInitProc = NULL, *tclVerProc = NULL;
    Ns_ModuleInitProc    *initProc = NULL;
    Ns_DString            ds;
    int                   status, *verPtr = NULL;
    Tcl_Obj              *pathObj;
    Tcl_Interp           *interp;
    Tcl_LoadHandle        lh;
    Tcl_FSUnloadFileProc *uPtr;

    Ns_Log(Notice, "modload: loading %s", file);

    Ns_DStringInit(&ds);
    if (!Ns_PathIsAbsolute(file)) {
        file = Ns_HomePath(&ds, "bin", file, NULL);
    }
    pathObj = Tcl_NewStringObj(file, -1);

    Tcl_IncrRefCount(pathObj);
    if (Tcl_FSGetNormalizedPath(NULL, pathObj) == NULL) {
        Ns_Log(Error, "modload: %s: invalid path", file);
        Tcl_DecrRefCount(pathObj);
        Ns_DStringFree(&ds);
        return NS_ERROR;
    }

    interp = Tcl_CreateInterp();
    status = Tcl_FSLoadFile(interp, pathObj, init, "Ns_ModuleVersion",
                            &tclInitProc, &tclVerProc, &lh, &uPtr);
    Tcl_DecrRefCount(pathObj);
    if (status != TCL_OK) {
        Ns_Log(Error, "modload: %s: %s", file, Tcl_GetStringResult(interp));
        Tcl_DeleteInterp(interp);
        Ns_DStringFree(&ds);
        return NS_ERROR;
    }
    Tcl_DeleteInterp(interp);

    initProc = (Ns_ModuleInitProc *) tclInitProc;
    verPtr = (int *) tclVerProc;

    if (initProc == NULL) {
        Ns_Log(Error, "modload: %s: %s: symbol not found", file, init);
        Ns_DStringFree(&ds);
        return NS_ERROR;
    }

    status = (*initProc)(server, module);

    if (verPtr == NULL || *verPtr < 1) {
        status = NS_OK;
    } else if (status != NS_OK) {
        Ns_Log(Error, "modload: %s: %s returned: %d", file, init, status);
    }

    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclModuleLoadObjCmd --
 *
 *      Implements ns_moduleload.  Load and initilize a binary module.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Will exit the server with a fatal error if module fails to load
 *      initialize correctly.
 *
 *----------------------------------------------------------------------
 */

int
NsTclModuleLoadObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp   *itPtr = (NsInterp *) arg;
    CONST char *server, *module, *file, *init = "Ns_ModuleInit";
    int         global = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-global", Ns_ObjvBool,   &global, (void *) NS_TRUE},
        {"-init",   Ns_ObjvString, &init,   NULL},
        {"--",      Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"module",  Ns_ObjvString, &module, NULL},
        {"file",    Ns_ObjvString, &file,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (Ns_InfoStarted()) {
        Tcl_SetResult(interp, "server already started", TCL_STATIC);
        return TCL_ERROR;
    }
    if (global) {
        server = NULL;
    } else {
        server = itPtr->servPtr->server;
    }
    if (Ns_ModuleLoad(server, module, file, init) != NS_OK) {
        Ns_Fatal("modload: failed to load module '%s'", file);
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitStaticModules --
 *
 *      Initialize static modules for given server, or global static
 *      modules if no server given.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Static modules may register new static modules, so we loop
 *      until they're all gone.
 *
 *----------------------------------------------------------------------
 */

void 
NsInitStaticModules(CONST char *server)
{
    Module *modPtr, *nextPtr;

    while (firstPtr != NULL) {
        modPtr = firstPtr;
        firstPtr = NULL;
        while (modPtr != NULL) {
            nextPtr = modPtr->nextPtr;
            Ns_Log(Notice, "modload: %s: initializing module", modPtr->name);
            if ((*modPtr->proc)(server, modPtr->name) != NS_OK) {
                Ns_Fatal("modload: %s: failed to initialize", modPtr->name);
            }
            ns_free(modPtr->name);
            ns_free(modPtr);
            modPtr = nextPtr;
        }
    }
}
