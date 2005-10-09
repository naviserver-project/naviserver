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

static Tcl_HashTable modulesTable; /* Table of loaded, loadable modules. */
static Module *firstPtr;           /* List of static modules to be inited. */


/*
 *----------------------------------------------------------------------
 *
 * NsInitModLoad --
 *
 *      Initialize module table.
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
NsInitModLoad(void)
{
#ifdef _WIN32
    Tcl_InitHashTable(&modulesTable, TCL_STRING_KEYS);
#else
    Tcl_InitHashTable(&modulesTable, FILE_KEYS);
#endif
}


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
 *      Load a module and initialize it.  The result code from modules
 *      without the version symbol are ignored.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ModuleLoad(CONST char *server, CONST char *module, CONST char *file,
              CONST char *init)
{
    Tcl_PackageInitProc  *tclInitProc = NULL, *tclVerProc = NULL;
    Ns_ModuleInitProc    *initProc = NULL;
    int                   status, *verPtr = NULL;
    Tcl_Obj              *pathObj;
    Tcl_Interp           *interp;
    Tcl_LoadHandle        lh;
    Tcl_FSUnloadFileProc *uPtr;

    Ns_Log(Notice, "modload: loading %s", file);

    pathObj = Tcl_NewStringObj(file, -1);
    Tcl_IncrRefCount(pathObj);
    if (Tcl_FSGetNormalizedPath(NULL, pathObj) == NULL) {
        Tcl_DecrRefCount(pathObj);
        Ns_Log(Error, "modload: %s: invalid path", file);
        return NS_ERROR;
    }

    interp = Ns_TclAllocateInterp(server);
    if (interp == NULL) {
        Ns_Log(Error, "modload: invalid server name: '%s'", server);
        return NS_ERROR;
    }
    status = Tcl_FSLoadFile(interp, pathObj, init, "Ns_ModuleVersion",
                            &tclInitProc, &tclVerProc, &lh, &uPtr);
    Tcl_DecrRefCount(pathObj);
    if (status != TCL_OK) {
        Ns_Log(Error, "modload: %s: %s", file, Tcl_GetStringResult(interp));
        Ns_TclDeAllocateInterp(interp);
        return NS_ERROR;
    }
    Ns_TclDeAllocateInterp(interp);

    initProc = (Ns_ModuleInitProc *) tclInitProc;
    verPtr = (int *) tclVerProc;

    if (initProc == NULL) {
        Ns_Log(Error, "modload: %s: %s: symbol not found", file, init);
        return NS_ERROR;
    }
    status = (*initProc)(server, module);
    if (verPtr == NULL || *verPtr < 1) {
        status = NS_OK;
    } else if (status != NS_OK) {
        Ns_Log(Error, "modload: %s: %s returned: %d", file, init, status);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsLoadModules --
 *
 *      Load all modules for given server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will load and initialize modules.
 *
 *----------------------------------------------------------------------
 */

void 
NsLoadModules(CONST char *server)
{
    Ns_Set *modules;
    int     i;
    char   *file, *module, *init = NULL, *s, *e = NULL;
    Module *modPtr, *nextPtr;

    modules = Ns_ConfigGetSection(Ns_ConfigGetPath(server, NULL,
                                                   "modules", NULL));
    for (i = 0; modules != NULL && i < Ns_SetSize(modules); ++i) {
        module = Ns_SetKey(modules, i);
        file = Ns_SetValue(modules, i);

        /*
         * Check for specific module init after filename.
         */

        s = strchr(file, '(');
        if (s == NULL) {
            init = "Ns_ModuleInit";
        } else {
            *s = '\0';
            init = s + 1;
            e = strchr(init, ')');
            if (e != NULL) {
                *e = '\0';
            }
        }

        /*
         * Load the module if it's not the reserved "tcl" name.
         */
        
        if (!STRIEQ(file, "tcl")
            && Ns_ModuleLoad(server, module, file, init) != NS_OK) {
            Ns_Fatal("modload: %s: failed to load module", file);
        }

        /*
         * Add this module to the server Tcl init list.
         */

        Ns_TclInitModule(server, module);

        if (s != NULL) {
            *s = '(';
            if (e != NULL) {
                *e = ')';
            }
        }
    }

    /*
     * Initialize the static modules (if any).  Note that a static
     * module could add a new static module and so the loop is
     * repeated until they're all gone.
     */

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
