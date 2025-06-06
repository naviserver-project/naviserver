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
 * url2file.c --
 *
 *      Routines to register, unregister, and run url2file callbacks.
 */

#include "nsd.h"

/*
 * The following structure defines a url2file callback including user
 * routine and client data.
 */

typedef struct {
    int                    refcnt;
    Ns_Url2FileProc       *proc;
    Ns_Callback           *deleteCallback;
    void                  *arg;
    unsigned int           flags;
} Url2File;

/*
 * The following structure defines a mount point for a URL path.
 */

typedef struct {
    char       *basepath;
    char       *url;
    const char *server;
} Mount;


/*
 * Static functions defined in this file.
 */

static Ns_Callback FreeMount;
static void FreeUrl2File(void *arg);
static Ns_ArgProc WalkCallback;
static Ns_ServerInitProc ConfigServerUrl2File;


/*
 * Static variables defined in this file.
 */

static Ns_Mutex   ulock = NULL;
static int        uid;


/*
 *----------------------------------------------------------------------
 *
 * NsInitUrl2File --
 *
 *      Initialize the url2file API.
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
NsInitUrl2File(void)
{
    uid = Ns_UrlSpecificAlloc();
    Ns_MutexInit(&ulock);
    Ns_MutexSetName(&ulock, "nsd:url2file");

    NsRegisterServerInit(ConfigServerUrl2File);
}

static Ns_ReturnCode
ConfigServerUrl2File(const char *server)
{
    NsServer *servPtr;
    Ns_ReturnCode result;

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Ns_RegisterUrl2FileProc(server, "/", Ns_FastUrl2FileProc, NULL, servPtr, 0u);
        /*
         * The following call was here for quite a long time, using the
         * deprecated Ns_SetUrlToFileProc() (at least since 2005). So, code
         * might depend on this, but I doubt it. Leave it for the time being,
         * commented out.
         */
        //Ns_SetUrlToFileProc(server, NsUrlToFileProc);
        result = NS_OK;
    } else {
        result = NS_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterUrl2FileProc --
 *
 *      Register a new procedure that acts like Ns_UrlToFile to service
 *      matching url pattern.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The delete procedure of previously registered request, if any,
 *      will be called unless NS_OP_NODELETE flag is set.
 *      Overrides any procedure registered with Ns_SetUrlToFileProc().
 *
 *----------------------------------------------------------------------
 */

void
Ns_RegisterUrl2FileProc(const char *server, const char *url,
                        Ns_Url2FileProc *proc, Ns_Callback *deleteCallback, void *arg,
                        unsigned int flags)
{
    NsServer *servPtr = NsGetServer(server);

    if (servPtr != NULL) {
        Url2File *u2fPtr;

        servPtr->fastpath.url2file = NULL;
        u2fPtr = ns_malloc(sizeof(Url2File));
        u2fPtr->proc = proc;
        u2fPtr->deleteCallback = deleteCallback;
        u2fPtr->arg = arg;
        u2fPtr->flags = flags;
        u2fPtr->refcnt = 1;
        Ns_MutexLock(&ulock);
        Ns_UrlSpecificSet(server, "x", url, uid, u2fPtr, flags, FreeUrl2File);
        Ns_MutexUnlock(&ulock);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UnRegisterUrl2FileProc --
 *
 *      Remove the procedure which matches the given url pattern.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registered peocedure's deleteProc may run.
 *
 *----------------------------------------------------------------------
 */

void
Ns_UnRegisterUrl2FileProc(const char *server, const char *url, unsigned int flags)
{
    Ns_MutexLock(&ulock);
    (void) Ns_UrlSpecificDestroy(server, "x", url, uid, flags);
    Ns_MutexUnlock(&ulock);
}


/*
 *----------------------------------------------------------------------
 * Ns_FastUrl2FileProc --
 *
 *      Construct a pathname relative to the server pages directory.
 *
 * Results:
 *      NS_OK or NS_ERROR if the NsPageRoot() proc fails.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_FastUrl2FileProc(Tcl_DString *dsPtr, const char *url, const void *arg)
{
    Ns_ReturnCode   status = NS_OK;
    const NsServer *servPtr = arg;

    if (NsPageRoot(dsPtr, servPtr, NULL) == NULL) {
        status = NS_ERROR;
    } else {
        (void) Ns_MakePath(dsPtr, url, NS_SENTINEL);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlToFile, NsUrlToFile --
 *
 *      Construct the filename that corresponds to a URL.
 *
 * Results:
 *      Return NS_OK on success or NS_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_UrlToFile(Tcl_DString *dsPtr, const char *server, const char *url)
{
    NsServer      *servPtr;
    Ns_ReturnCode  status;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        status = NsUrlToFile(dsPtr, servPtr, url);
    } else {
        status = NS_ERROR;
    }

    return status;
}

Ns_ReturnCode
NsUrlToFile(Tcl_DString *dsPtr, NsServer *servPtr, const char *url)
{
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    if (servPtr->fastpath.url2file != NULL) {
        Ns_Log(Debug, "url2file: url '%s' use fastpath.url2file", url);
        status = (*servPtr->fastpath.url2file)(dsPtr, servPtr->server, url);
    } else {
        Url2File *u2fPtr;

        Ns_Log(Debug, "url2file: url '%s' use Ns_UrlSpecificGet to determine filename", url);

        Ns_MutexLock(&ulock);
        u2fPtr = Ns_UrlSpecificGet((Ns_Server*)servPtr, "x", url, uid, 0u,
                                   NS_URLSPACE_DEFAULT, NULL, NULL, NULL);
        if (u2fPtr == NULL) {
            Ns_Log(Error, "url2file: no proc found for url: %s", url);
            status = NS_ERROR;
        } else {
            ++u2fPtr->refcnt;
            Ns_MutexUnlock(&ulock);
            status = (*u2fPtr->proc)(dsPtr, url, u2fPtr->arg);
            Ns_MutexLock(&ulock);
            FreeUrl2File(u2fPtr);
        }
        Ns_MutexUnlock(&ulock);
    }
    if (status == NS_OK) {
        while (dsPtr->length > 0 && dsPtr->string[dsPtr->length -1] == '/') {
            Tcl_DStringSetLength(dsPtr, dsPtr->length -1);
        }
    }

    Ns_Log(Debug, "url2file: url '%s' is mapped to '%s' (status %d)", url, dsPtr->string, status);
    return status;
}

#ifdef NS_WITH_DEPRECATED
/*
 *----------------------------------------------------------------------
 * Ns_SetUrlToFileProc --
 *
 *      Set pointer to custom procedure that acts like Ns_UrlToFile().
 *
 *      Deprecated, use Ns_RegisterUrl2FileProc().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Overrides all existing procedures registered with new API.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetUrlToFileProc(const char *server, Ns_UrlToFileProc *procPtr)
{
    NsServer *servPtr = NsGetServer(server);

    Ns_Log(Warning, "DEPRECATED function Ns_SetUrlToFileProc called");
    if (servPtr != NULL) {
        servPtr->fastpath.url2file = procPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsUrlToFileProc --
 *
 *      Default old-style url2file proc registered at server startup.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsUrlToFileProc(Tcl_DString *dsPtr, const char *server, const char *url)
{
    const NsServer *servPtr = NsGetServer(server);
    Ns_ReturnCode   result;

    if (likely(servPtr != NULL)) {
        result = Ns_FastUrl2FileProc(dsPtr, url, servPtr);
    } else {
        result = NS_ERROR;
    }
    return result;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * NsTclUrl2FileObjCmd --
 *
 *      Implements "ns_url2file".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclUrl2FileObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/url/");
        result = TCL_ERROR;
    } else {
        Tcl_DString     ds;
        const NsInterp *itPtr = clientData;

        Tcl_DStringInit(&ds);
        if (NsUrlToFile(&ds, itPtr->servPtr, Tcl_GetString(objv[1])) != NS_OK) {
            Ns_TclPrintfResult(interp, "url2file lookup failed for %s", Tcl_GetString(objv[1]));
            Tcl_DStringFree(&ds);
            result = TCL_ERROR;
        } else {
            Tcl_DStringResult(interp, &ds);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterUrl2FileObjCmd --
 *
 *      Implements "ns_register_url2file".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See Ns_RegisterUrl2FileProc().
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterUrl2FileObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *url;
    Tcl_Obj    *scriptObj;
    TCL_SIZE_T  remain = 0;
    int         noinherit = 0, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,   &noinherit, INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",        Ns_ObjvString, &url,       NULL},
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?arg",       Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        unsigned int    flags;
        Ns_TclCallback *cbPtr;

        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclUrl2FileProc,
                                  scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
        flags = (noinherit != 0) ? NS_OP_NOINHERIT : 0u;
        Ns_RegisterUrl2FileProc(itPtr->servPtr->server, url,
                                NsTclUrl2FileProc, Ns_TclFreeCallback, cbPtr, flags);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUnRegisterUrl2FileObjCmd --
 *
 *      Implements "ns_unregister_url2file".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See Ns_UnRegisterUrlToFileProc().
 *
 *----------------------------------------------------------------------
 */

int
NsTclUnRegisterUrl2FileObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    char           *url = NULL;
    int             noinherit = 0, recurse = 0, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,   &noinherit, INT2PTR(NS_TRUE)},
        {"-recurse",   Ns_ObjvBool,   &recurse,   INT2PTR(NS_TRUE)},
        {"-server",    Ns_ObjvServer, &servPtr,   NULL},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",      Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        unsigned int    flags = 0u;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }
        if (recurse != 0)   {
            flags |= NS_OP_RECURSE;
        }

        Ns_UnRegisterUrl2FileProc(servPtr->server, url, flags);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterFastUrl2FileObjCmd --
 *
 *      Register an Ns_RegisterUrl2FileProc() with either the
 *      Ns_FastUrl2FileProc() or NsMountUrl2FileProc() depending on the
 *      provided basePath.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registered callback.
 *
 *----------------------------------------------------------------------
 */
void
Ns_RegisterFastUrl2File(const char *server, const char *url, const char *basePath, unsigned int flags)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        if (basePath == NULL) {
            Ns_RegisterUrl2FileProc(server, url,
                                    Ns_FastUrl2FileProc, NULL, servPtr, flags);
        } else {
            Mount *mPtr;

            mPtr = ns_malloc(sizeof(Mount));
            mPtr->basepath = ns_strdup(basePath);
            mPtr->url = ns_strdup(url);
            mPtr->server = server;
            Ns_RegisterUrl2FileProc(server, url,
                                    NsMountUrl2FileProc, FreeMount, mPtr, flags);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterFastUrl2FileObjCmd --
 *
 *      Implements "ns_register_fasturl2file".  Register the default fast
 *      url2file proc for the given URL.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclRegisterFastUrl2FileObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *url = NULL, *basepath = NULL;
    int         noinherit = 0, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &noinherit, INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",        Ns_ObjvString, &url,      NULL},
        {"?basepath",  Ns_ObjvString, &basepath, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        unsigned int    flags = 0u;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }
        Ns_RegisterFastUrl2File(itPtr->servPtr->server, url, basepath, flags);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUrl2FileProc --
 *
 *      Callback for Tcl url2file procs.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Depends on Tcl script.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsTclUrl2FileProc(Tcl_DString *dsPtr, const char *url, const void *arg)
{
    Ns_ReturnCode         status = NS_OK;
    const Ns_TclCallback *cbPtr = arg;

    if (unlikely(Ns_TclEvalCallback(NULL, cbPtr, dsPtr, url, NS_SENTINEL) != TCL_OK)) {
        status = NS_ERROR;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsMountUrl2FileProc --
 *
 *      Construct new path relative to registered basepath.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsMountUrl2FileProc(Tcl_DString *dsPtr, const char *url, const void *arg)
{
    Ns_ReturnCode status = NS_OK;
    const Mount  *mPtr = arg;
    const char   *u;

    u = mPtr->url;
    while (*u != '\0' && *url != '\0' && *u == *url) {
        ++u;
        ++url;
    }
    if (Ns_PathIsAbsolute(mPtr->basepath)) {
        Ns_MakePath(dsPtr, mPtr->basepath, url, NS_SENTINEL);
    } else if (Ns_PagePath(dsPtr, mPtr->server, mPtr->basepath, url, NS_SENTINEL) == NULL) {
        status = NS_ERROR;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsMountUrl2FileArgProc --
 *
 *      Info callback for procs which take Mount arg.
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
NsMountUrl2FileArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const Mount *mPtr = arg;

    Tcl_DStringAppendElement(dsPtr, mPtr->basepath);
    Tcl_DStringAppendElement(dsPtr, mPtr->url);
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetUrl2FileProcs --
 *
 *      Append information about registered url2file procs to dstring.
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
NsGetUrl2FileProcs(Tcl_DString *dsPtr, const char *server)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);

    Ns_MutexLock(&ulock);
    Ns_UrlSpecificWalk(uid, server, WalkCallback, dsPtr);
    Ns_MutexUnlock(&ulock);
}

static void
WalkCallback(Tcl_DString *dsPtr, const void *arg)
{
    const Url2File *u2fPtr = arg;

    Ns_GetProcInfo(dsPtr, (ns_funcptr_t)u2fPtr->proc, u2fPtr->arg);
}


/*
 *----------------------------------------------------------------------
 *
 * FreeMount --
 *
 *      Free Mount data.
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
FreeMount(void *arg)
{
    Mount *mPtr = arg;

    ns_free(mPtr->basepath);
    ns_free(mPtr->url);
    ns_free(mPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * FreeUrl2File --
 *
 *      Free Url2File data when reference count reaches 0.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on request delete procedure.
 *
 *----------------------------------------------------------------------
 */

static void
FreeUrl2File(void *arg)
{
    Url2File *u2fPtr = (Url2File *) arg;

    if (--u2fPtr->refcnt == 0) {
        if (u2fPtr->deleteCallback != NULL) {
            (*u2fPtr->deleteCallback)(u2fPtr->arg);
        }
        ns_free(u2fPtr);
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
