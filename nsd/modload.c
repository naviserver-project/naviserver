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
 * modload.c --
 *
 *      Load module files into the server and initialize them.
 */

#include "nsd.h"

/*
 * The following structure is used for static module loading.
 */

typedef struct Module {
    struct Module     *nextPtr;
    const char        *name;
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
 *      an Ns_ServerInitProc passed to Ns_Main or within the Ns_ModuleInit
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
Ns_RegisterModule(const char *name, Ns_ModuleInitProc *proc)
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

Ns_ReturnCode
Ns_ModuleLoad(Tcl_Interp *interp, const char *server, const char *module, const char *file,
              const char *init)
{
    Ns_DString            ds;
    Ns_ReturnCode         status = NS_OK;
    Tcl_Obj              *pathObj;
    bool                  hasExtension;

    NS_NONNULL_ASSERT(module != NULL);
    NS_NONNULL_ASSERT(file != NULL);
    NS_NONNULL_ASSERT(init != NULL);

    Ns_Log(Notice, "modload: loading module %s from file %s", module, file);

    Ns_DStringInit(&ds);
    if (Ns_PathIsAbsolute(file) == NS_FALSE) {
        file = Ns_BinPath(&ds, file, NS_SENTINEL);
    }
    /*
     * In the case of the nsproxy module, we have an "nsproxy" binary and an
     * "nsproxy.so" module. So the module file needs to have an extension.
     */
    hasExtension = (strrchr(file, INTCHAR('.')) != NULL);

    if (access(file, F_OK) != 0 || !hasExtension) {
        const char *defaultExtension =
#ifdef _WIN32
            ".dll"
#else
            ".so"
#endif
            ;
        TCL_SIZE_T extLength = (TCL_SIZE_T)strlen(defaultExtension);

        /*
         * The specified module name does not exist.  Try appending the
         * defaultExtension, but first make sure, we have the filename in the
         * Tcl_DString.
         */
        if (ds.length == 0) {
            Tcl_DStringAppend(&ds, file, TCL_INDEX_NONE);
        }

        /*
         * Avoid to append the extension twice.
         */
        if (ds.length > extLength
            && strncmp(defaultExtension, &ds.string[ds.length-extLength], (size_t)extLength) != 0
            ) {
            Tcl_DStringAppend(&ds, defaultExtension, extLength);
            file = ds.string;
        }
    }
    pathObj = Tcl_NewStringObj(file, TCL_INDEX_NONE);

    Tcl_IncrRefCount(pathObj);
    if (Tcl_FSGetNormalizedPath(NULL, pathObj) == NULL) {
        Ns_Log(Error, "modload: %s: invalid path", file);
        Tcl_DecrRefCount(pathObj);
        status = NS_ERROR;

    } else {
        Tcl_PackageInitProc  *tclInitProc = NULL, *moduleVersionAddr = NULL;
        int                   rc;
        bool                  privateInterp = (interp == NULL);
        Tcl_LoadHandle        lh = NULL;
        Tcl_FSUnloadFileProc *uPtr;

        if (privateInterp) {
            interp = NsTclCreateInterp();
        }
        /*
         * The 3rd arg of Tcl_FSLoadFile is the 1st symbol, typically
         * "Ns_ModuleInit".  The 4th arg of Tcl_FSLoadFile is the 2nd
         * symbol, hardcoded here to "Ns_ModuleVersion".
         *
         * Note that this is a little bit hacky, since the intention of
         * the Tcl interface is to return here the safeInitProc, which is
         * a procPtr and not a pointer to a global variable (object pointer).
         */
        rc = Tcl_FSLoadFile(interp, pathObj, init, "Ns_ModuleVersion",
                            &tclInitProc, &moduleVersionAddr, &lh, &uPtr);

        Tcl_DecrRefCount(pathObj);

        if (rc != TCL_OK) {
            Ns_Log(Error, "modload: %s: %s", file, Tcl_GetStringResult(interp));
            if (privateInterp) {
                Tcl_DeleteInterp(interp);
            }
            status = NS_ERROR;

        } else {
            if (privateInterp) {
                Tcl_DeleteInterp(interp);
            }

            if (tclInitProc == NULL) {
                Ns_Log(Error, "modload: %s: %s: symbol not found", file, init);
                status = NS_ERROR;
            }
            if (moduleVersionAddr == NULL) {
                Ns_Log(Error, "modload: %s: %s: symbol not found", file, "Ns_ModuleVersion");
                status = NS_ERROR;
            }
            if (status == NS_OK) {
                Ns_ModuleInitProc *initProc = (Ns_ModuleInitProc *)(ns_funcptr_t)tclInitProc;

                /*
                 * Calling Ns_ModuleInit()
                 */
                status = (*initProc)(server, module);
#if 0
                /*
                 * All modules of the NaviServer modules family have
                 * Ns_ModuleVersion == 1. The way, how AOLserver performed the
                 * version checking (via casting a function pointer to an
                 * integer pointer) does not conform with ISO C which forbids
                 * conversions of function pointers to object pointer
                 * types. The intention was probably to allow results from the
                 * initProc != NS_OK for modules with versions < 1.
                 *
                 * Since the test is practically useless und unclean, we
                 * deactivate it here.
                 */
                {
                    const int *versionPtr = (const int *) moduleVersionAddr;

                    if (*versionPtr < 1) {
                        status = NS_OK;
                    }
                }
#endif
                if (status != NS_OK) {
                    Ns_Log(Error, "modload: %s: %s returned: %d", file, init, status);
                }
            }
        }
    }
    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclModuleLoadObjCmd --
 *
 *      Implements "ns_moduleload".  Load and initialize a binary module.
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
NsTclModuleLoadObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char         *module, *file, *init = (char *)"Ns_ModuleInit";
    int           global = (int)NS_FALSE, result = TCL_OK;
    Ns_ObjvSpec   opts[] = {
        {"-global", Ns_ObjvBool,   &global, INT2PTR(NS_TRUE)},
        {"-init",   Ns_ObjvString, &init,   NULL},
        {"--",      Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   args[] = {
        {"module",  Ns_ObjvString, &module, NULL},
        {"file",    Ns_ObjvString, &file,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_InfoStarted()) {
        Ns_TclPrintfResult(interp, "server already started");
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        const char     *server;

        if (global == (int)NS_TRUE) {
            server = NULL;
        } else {
            server = itPtr->servPtr->server;
        }

        if (Ns_ModuleLoad(interp, server, module, file, init) != NS_OK) {
            Ns_Fatal("modload: failed to load module '%s'", file);
        }
    }

    return result;
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
NsInitStaticModules(const char *server)
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
            ns_free((char *)modPtr->name);
            ns_free(modPtr);
            modPtr = nextPtr;
        }
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
