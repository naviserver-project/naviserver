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

#ifdef NS_WITH_DEPRECATED
/*
 * The following structure is used for static module loading.
 */
typedef struct Module {
    struct Module     *nextPtr;
    const char        *name;
    Ns_ModuleInitProc *proc;
} Module;
#endif

/*
 * Scope of a loaded module. Global modules apply to every virtual
 * server, server modules to one server, and NSDB modules are database
 * drivers loaded indirectly by the nsdb module.
 */
typedef enum {
    NS_MODULE_SCOPE_GLOBAL,
    NS_MODULE_SCOPE_SERVER,
    NS_MODULE_SCOPE_NSDB
} ModuleScope;

/*
 * String representations of module scopes used in Tcl results.
 */
static Ns_ObjvTable moduleScopes[] = {
    {"global",   NS_MODULE_SCOPE_GLOBAL},
    {"server",   NS_MODULE_SCOPE_SERVER},
    {"nsdb",     NS_MODULE_SCOPE_NSDB},
    {NULL,       0u}
};

/*
 * Collected information about a successfully loaded native module.
 * All string values and module metadata stored here are owned by the
 * registry. servPtr is set only for server-scoped modules.
 */
typedef struct LoadedModule {
    const NsServer       *servPtr;
    ModuleScope           scope;
    char                 *module;
    char                 *driver;
    char                 *file;
    Ns_ModuleInfo         info;
    struct LoadedModule  *nextPtr;
} LoadedModule;

static Ns_Mutex       loadedModulesLock = NULL;
static LoadedModule  *firstLoadedModulePtr = NULL;
static LoadedModule **nextLoadedModulePtrPtr = &firstLoadedModulePtr;

/*
 * Static variables defined in this file.
 */
#ifdef NS_WITH_DEPRECATED
static Module *firstPtr;           /* List of static modules to be inited. */
#endif

/*
 * Static functions defined in this file.
 */
static void ModuleInfoCopy(Ns_ModuleInfo *destPtr, const Ns_ModuleInfo *sourcePtr)
    NS_GNUC_NONNULL(1);

static bool ModuleInfoValid(const Ns_ModuleInfo *infoPtr);

static void DictPutString(Tcl_Obj *dictObj, const char *key, const char *value)
    NS_GNUC_NONNULL(1,2);


static void
RegisterLoadedModule(const char *server, const char *module,
                     const char *file, const char *init,
                     const Ns_ModuleInfo *infoPtr)
    NS_GNUC_NONNULL(2,3,4);

#ifdef NS_WITH_DEPRECATED
/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterModule --
 *
 *      Deprecated legacy interface for registering a static module
 *      initialization callback. This mechanism predates the current
 *      module-loading infrastructure and has no in-tree callers.
 *
 *      It is retained for compatibility with external applications that
 *      may still register statically linked modules. New code should use
 *      the current module or server initialization interfaces.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates and appends an entry to the legacy static-module
 *      initialization list consumed by NsInitStaticModules().
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
#endif

/*
 *----------------------------------------------------------------------
 *
 * ModuleInfoCopy --
 *
 *      Copy module metadata into NaviServer-owned storage. Only fields
 *      present in the source structure, as indicated by its size, are
 *      accessed. String values are duplicated and owned by the
 *      destination record.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes the destination structure and allocates copies of
 *      available string values.
 *
 *----------------------------------------------------------------------
 */
static void
ModuleInfoCopy(Ns_ModuleInfo *destPtr, const Ns_ModuleInfo *sourcePtr)
{
    memset(destPtr, 0, sizeof(Ns_ModuleInfo));
    destPtr->size = sizeof(Ns_ModuleInfo);

    if (sourcePtr == NULL) {
        return;
    }

    if (NS_MODULE_INFO_HAS(sourcePtr, infoVersion)) {
        destPtr->infoVersion = sourcePtr->infoVersion;
    }
    if (NS_MODULE_INFO_HAS(sourcePtr, moduleVersion)) {
        destPtr->moduleVersion = sourcePtr->moduleVersion;
    }
    if (NS_MODULE_INFO_HAS(sourcePtr, name)) {
        destPtr->name = ns_strcopy(sourcePtr->name);
    }
    if (NS_MODULE_INFO_HAS(sourcePtr, version)) {
        destPtr->version = ns_strcopy(sourcePtr->version);
    }
    if (NS_MODULE_INFO_HAS(sourcePtr, tag)) {
        destPtr->tag = ns_strcopy(sourcePtr->tag);
    }
    if (NS_MODULE_INFO_HAS(sourcePtr, type) && sourcePtr->type != NULL) {
        destPtr->type = ns_strcopy(sourcePtr->type);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ModuleInfoValid --
 *
 *      Validate module metadata returned by Ns_ModuleGetInfo(). Check
 *      that the structure contains all required version-1 fields, uses
 *      the supported information version, and provides the required
 *      name, version, and tag values.
 *
 * Results:
 *      True when the module information is valid, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
ModuleInfoValid(const Ns_ModuleInfo *infoPtr)
{
    return infoPtr != NULL
        && NS_MODULE_INFO_HAS(infoPtr, type)
        && infoPtr->infoVersion > 0u
        && infoPtr->infoVersion <= NS_MODULE_INFO_VERSION
        && infoPtr->name != NULL
        && infoPtr->version != NULL
        && infoPtr->tag != NULL
        && infoPtr->type != NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * DictPutString --
 *
 *      Add a string value to a Tcl dictionary under the specified key.
 *      When value is NULL, no dictionary entry is added.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies the provided dictionary object and allocates Tcl objects
 *      for the key and value.
 *
 *----------------------------------------------------------------------
 */
static void
DictPutString(Tcl_Obj *dictObj, const char *key, const char *value)
{
    if (value != NULL) {
        (void) Tcl_DictObjPut(NULL, dictObj,
                              Tcl_NewStringObj(key, TCL_INDEX_NONE),
                              Tcl_NewStringObj(value, TCL_INDEX_NONE));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsGetLoadedModulesObj --
 *
 *      Build a dictionary describing the modules available to the
 *      specified server. Initially, modules registered in the server's
 *      Tcl module list are represented as Tcl modules. Entries for
 *      loaded native modules then replace or extend these provisional
 *      entries with build metadata, scope, file, and ABI information.
 *
 *      Global and NSDB modules are included for every server, while
 *      server-scoped modules are included only for their associated
 *      server. The resulting dictionary is sorted by module name to
 *      provide stable output.
 *
 * Results:
 *      Returns a newly allocated Tcl dictionary object containing the
 *      module information.
 *
 * Side effects:
 *      Allocates Tcl objects and temporarily locks the loaded-module
 *      registry while native module records are inspected.
 *
 *----------------------------------------------------------------------
 */
Tcl_Obj *
NsGetLoadedModulesObj(const NsServer *servPtr)
{
    const LoadedModule *loadedPtr;
    Tcl_Obj             *resultObj, *sortedObj;
    const char         **argv;
    TCL_SIZE_T           argc;

    NS_NONNULL_ASSERT(servPtr != NULL);

    resultObj = Tcl_NewDictObj();

    if (Tcl_SplitList(NULL, servPtr->tcl.modules.string, &argc, &argv) == TCL_OK) {
        for (TCL_SIZE_T i = 0; i < argc; i++) {
            const char *module  = argv[i];
            Tcl_Obj    *infoObj = Tcl_NewDictObj();

            DictPutString(infoObj, "name", module);
            DictPutString(infoObj, "type", "tcl-module");
            DictPutString(infoObj, "scope", "server");

            (void) Tcl_DictObjPut(NULL, resultObj,
                                  Tcl_NewStringObj(module, TCL_INDEX_NONE),
                                  infoObj);
        }
        Tcl_Free((char *)argv);
    }

    Ns_MutexLock(&loadedModulesLock);

    for (loadedPtr = firstLoadedModulePtr;
         loadedPtr != NULL;
         loadedPtr = loadedPtr->nextPtr) {
        Tcl_Obj *infoObj;

        switch (loadedPtr->scope) {
        case NS_MODULE_SCOPE_GLOBAL:
        case NS_MODULE_SCOPE_NSDB:
            /*
             * Visible for every server.
             */
            break;

        case NS_MODULE_SCOPE_SERVER:
            if (loadedPtr->servPtr != servPtr) {
                continue;
            }
            break;
        }

        infoObj = Tcl_NewDictObj();

        DictPutString(infoObj, "name",
                      loadedPtr->info.name != NULL
                      ? loadedPtr->info.name
                      : loadedPtr->module);

        DictPutString(infoObj, "type",
                      loadedPtr->info.type != NULL
                      ? loadedPtr->info.type
                      : "module");

        DictPutString(infoObj, "version", loadedPtr->info.version);
        DictPutString(infoObj, "tag", loadedPtr->info.tag);
        DictPutString(infoObj, "scope", Ns_ObjvTableGetString(moduleScopes, loadedPtr->scope));
        DictPutString(infoObj, "file", loadedPtr->file);
        DictPutString(infoObj, "driver", loadedPtr->driver);

        if (loadedPtr->info.infoVersion != 0u) {
            (void) Tcl_DictObjPut(
                NULL, infoObj,
                Tcl_NewStringObj("infoversion", TCL_INDEX_NONE),
                Tcl_NewWideIntObj(
                    (Tcl_WideInt)loadedPtr->info.infoVersion));
        }

        if (loadedPtr->info.moduleVersion != 0u) {
            (void) Tcl_DictObjPut(
                NULL, infoObj,
                Tcl_NewStringObj("abi", TCL_INDEX_NONE),
                Tcl_NewWideIntObj(
                    (Tcl_WideInt)loadedPtr->info.moduleVersion));
        }

        (void) Tcl_DictObjPut(
            NULL, resultObj,
            Tcl_NewStringObj(loadedPtr->module, TCL_INDEX_NONE),
            infoObj);
    }

    Ns_MutexUnlock(&loadedModulesLock);

    Tcl_IncrRefCount(resultObj);
    sortedObj = NsTclDictSort(NULL, resultObj);

    if (sortedObj != NULL) {
        Tcl_DecrRefCount(resultObj);
        return sortedObj;
    }

    Tcl_DecrRefCount(resultObj);
    return Tcl_NewDictObj();
}

/*
 *----------------------------------------------------------------------
 *
 * RegisterLoadedModule --
 *
 *      Register a successfully initialized native module in the
 *      persistent loaded-module registry. Determine its scope from the
 *      loading context, copy the module name and file path, and copy any
 *      metadata returned by Ns_ModuleGetInfo().
 *
 *      Modules initialized through Ns_DbDriverInit are recorded as NSDB
 *      drivers, modules loaded without a server as global, and all other
 *      modules as belonging to the specified server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates and appends a LoadedModule record and copies the
 *      supplied strings and metadata. Locks the loaded-module registry
 *      while linking the new record.
 *
 *----------------------------------------------------------------------
 */
static void
RegisterLoadedModule(const char *server, const char *module,
                     const char *file, const char *init,
                     const Ns_ModuleInfo *infoPtr)
{
    LoadedModule *loadedPtr;

    loadedPtr = ns_calloc(1u, sizeof(LoadedModule));

    if (strcmp(init, "Ns_DbDriverInit") == 0) {
        loadedPtr->scope = NS_MODULE_SCOPE_NSDB;
        loadedPtr->servPtr = NULL;
        loadedPtr->driver = ns_strcopy(server);
    } else if (server == NULL) {
        loadedPtr->scope = NS_MODULE_SCOPE_GLOBAL;
        loadedPtr->servPtr = NULL;
    } else {
        loadedPtr->scope = NS_MODULE_SCOPE_SERVER;
        loadedPtr->servPtr = NsGetServer(server);
    }

    /*
     * Resolve the supplied value to an actual NaviServer server.
     *
     * This deliberately does not retain an unresolved value such as
     * "postgres", which is passed as the second argument by
     * NsDbLoadDriver().
     */
    loadedPtr->module = ns_strcopy(module);
    loadedPtr->file = ns_strcopy(file);

    ModuleInfoCopy(&loadedPtr->info, infoPtr);

    Ns_MutexLock(&loadedModulesLock);

    loadedPtr->nextPtr = NULL;
    *nextLoadedModulePtrPtr = loadedPtr;
    nextLoadedModulePtrPtr = &loadedPtr->nextPtr;

    Ns_MutexUnlock(&loadedModulesLock);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInfoInit --
 *
 *      Initialize module metadata returned by Ns_ModuleGetInfo().
 *      Populate only fields present in the caller-provided structure,
 *      as indicated by its size, to preserve compatibility between
 *      different module information versions.
 *
 *      The supplied strings are referenced directly and must remain
 *      valid for the lifetime of the loaded module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates the fields available in the provided Ns_ModuleInfo
 *      structure. Does not allocate or copy string values.
 *
 *----------------------------------------------------------------------
 */
void
Ns_ModuleInfoInit(
    Ns_ModuleInfo *infoPtr,
    unsigned int infoVersion,
    const char *name,
    const char *version,
    const char *tag,
    const char *type,
    unsigned int moduleVersion)
{
    if (NS_MODULE_INFO_HAS(infoPtr, infoVersion)) {
        infoPtr->infoVersion = infoVersion;
    }
    if (NS_MODULE_INFO_HAS(infoPtr, moduleVersion)) {
        infoPtr->moduleVersion = moduleVersion;
    }
    if (NS_MODULE_INFO_HAS(infoPtr, name)) {
        infoPtr->name = name;
    }
    if (NS_MODULE_INFO_HAS(infoPtr, version)) {
        infoPtr->version = version;
    }
    if (NS_MODULE_INFO_HAS(infoPtr, tag)) {
        infoPtr->tag = tag;
    }
    if (NS_MODULE_INFO_HAS(infoPtr, type)) {
        infoPtr->type = type;
    }
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
    Tcl_DString           ds;
    Ns_ReturnCode         status = NS_OK;
    Tcl_Obj              *pathObj;
    bool                  hasExtension;

    NS_NONNULL_ASSERT(module != NULL);
    NS_NONNULL_ASSERT(file != NULL);
    NS_NONNULL_ASSERT(init != NULL);

    Ns_Log(Notice, "modload: loading module %s from file %s", module, file);

    Tcl_DStringInit(&ds);
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
            Ns_ModuleInfoProc *infoProc = NULL;
            bool               hasModuleInfo = NS_FALSE;
            Ns_ModuleInfo      moduleInfo;

            memset(&moduleInfo, 0, sizeof(moduleInfo));
            moduleInfo.size = sizeof(moduleInfo);

#ifndef NS_TCL_PRE86
            {
                void *symbol = Tcl_FindSymbol(interp, lh, "Ns_ModuleGetInfo");
                if (symbol != NULL) {
                    memcpy(&infoProc, &symbol, sizeof(infoProc));
                }
            }
#endif
            if (infoProc != NULL) {
                (*infoProc)(&moduleInfo);

                if (ModuleInfoValid(&moduleInfo)) {
                    hasModuleInfo = NS_TRUE;
                } else {
                    Ns_Log(Warning,
                           "modload: module %s from file %s returned invalid module information",
                           module, file);
                }
            } else {
                /*
                 * Clear a possible error from the optional symbol lookup.
                 */
                Tcl_ResetResult(interp);
            }

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
                if (status != NS_OK) {
                    Ns_Log(Error, "modload: %s: %s returned: %s", file, init, Ns_ReturnCodeString(status));
                } else {
                    RegisterLoadedModule(server, module, file, init,
                                         hasModuleInfo ? &moduleInfo : NULL);
                }
            }
        }
    }
    Tcl_DStringFree(&ds);

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
    char         *module, *file;
    const char   *init = "Ns_ModuleInit";
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

#ifdef NS_WITH_DEPRECATED
/*
 *----------------------------------------------------------------------
 *
 * NsInitStaticModules --
 *
 *      Process callbacks registered through the deprecated
 *      Ns_RegisterModule() interface. The stock NaviServer build has no
 *      producers for this list; the function is retained to support
 *      external users of the legacy static-module API.
 *
 *      Since callbacks may register additional callbacks, the lists are
 *      processed repeatedly until no entries remain.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Invokes and frees all entries in the legacy static-module list.
 *      Terminates the process when an initialization callback fails.
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
            ns_free_const(modPtr->name);
            ns_free(modPtr);
            modPtr = nextPtr;
        }
    }
}
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
