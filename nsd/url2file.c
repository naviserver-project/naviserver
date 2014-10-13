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
    CONST char *server;
} Mount;


/*
 * Static functions defined in this file.
 */

static Ns_Callback FreeMount;
static void FreeUrl2File(void *arg);
static void WalkCallback(Ns_DString *dsPtr, void *arg);
static Ns_ServerInitProc ConfigServerUrl2File;


/*
 * Static variables defined in this file.
 */

static Ns_Mutex   ulock;
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

static int
ConfigServerUrl2File(CONST char *server)
{
    NsServer *servPtr;

    servPtr = NsGetServer(server);
    Ns_RegisterUrl2FileProc(server, "/", Ns_FastUrl2FileProc, NULL, servPtr, 0);
    Ns_SetUrlToFileProc(server, NsUrlToFileProc);

    return NS_OK;
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
Ns_RegisterUrl2FileProc(CONST char *server, CONST char *url,
                        Ns_Url2FileProc *proc, Ns_Callback *deleteCallback, void *arg,
                        unsigned int flags)
{
    NsServer *servPtr = NsGetServer(server);
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
Ns_UnRegisterUrl2FileProc(CONST char *server, CONST char *url, unsigned int flags)
{
    Ns_MutexLock(&ulock);
    Ns_UrlSpecificDestroy(server, "x", url, uid, flags);
    Ns_MutexUnlock(&ulock);
}


/*
 *----------------------------------------------------------------------
 * Ns_FastUrl2FileProc --
 *
 *      Construct a path name relative to the server pages directory.
 *
 * Results:
 *      NS_OK or NS_ERROR if the pageroot proc fails.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_FastUrl2FileProc(Ns_DString *dsPtr, CONST char *url, void *arg)
{
    NsServer *servPtr = arg;

    if (NsPageRoot(dsPtr, servPtr, NULL) == NULL) {
        return NS_ERROR;
    }
    Ns_MakePath(dsPtr, url, NULL);

    return NS_OK;
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

int
Ns_UrlToFile(Ns_DString *dsPtr, CONST char *server, CONST char *url)
{
    NsServer *servPtr = NsGetServer(server);

    return NsUrlToFile(dsPtr, servPtr, url);
}

int
NsUrlToFile(Ns_DString *dsPtr, NsServer *servPtr, CONST char *url)
{
    int       status = NS_ERROR;

    if (url == NULL) {
        return status;
    }
    if (servPtr->fastpath.url2file != NULL) {
        status = (*servPtr->fastpath.url2file)(dsPtr, servPtr->server, url);
    } else {
        Url2File *u2fPtr;

        Ns_MutexLock(&ulock);
        u2fPtr = NsUrlSpecificGet(servPtr, "x", url, uid, 0);
        if (u2fPtr == NULL) {
            Ns_MutexUnlock(&ulock);
            Ns_Log(Error, "url2file: no proc found for url: %s", url);
            return NS_ERROR;
        }
        ++u2fPtr->refcnt;
        Ns_MutexUnlock(&ulock);
        status = (*u2fPtr->proc)(dsPtr, url, u2fPtr->arg);
        Ns_MutexLock(&ulock);
        FreeUrl2File(u2fPtr);
        Ns_MutexUnlock(&ulock);
    }
    if (status == NS_OK) {
        while (dsPtr->length > 0 && dsPtr->string[dsPtr->length -1] == '/') {
            Ns_DStringSetLength(dsPtr, dsPtr->length -1);
        }
    }

    return status;
}


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
Ns_SetUrlToFileProc(CONST char *server, Ns_UrlToFileProc *procPtr)
{
    NsServer *servPtr = NsGetServer(server);

    servPtr->fastpath.url2file = procPtr;
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

int
NsUrlToFileProc(Ns_DString *dsPtr, CONST char *server, CONST char *url)
{
    NsServer *servPtr = NsGetServer(server);

    return Ns_FastUrl2FileProc(dsPtr, url, servPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUrl2FileObjCmd --
 *
 *      Implements ns_url2file as obj command. 
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
NsTclUrl2FileObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp   *itPtr = arg;
    Ns_DString  ds;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "url");
        return TCL_ERROR;
    }
    Ns_DStringInit(&ds);
    if (NsUrlToFile(&ds, itPtr->servPtr, Tcl_GetString(objv[1])) != NS_OK) {
        Tcl_SetResult(interp, "url2file lookup failed", TCL_STATIC);
        Ns_DStringFree(&ds);
        return TCL_ERROR;
    }
    Tcl_DStringResult(interp, &ds);
    Ns_DStringFree(&ds);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterUrl2FileObjCmd --
 *
 *      Implements ns_register_url2file as obj command. 
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
NsTclRegisterUrl2FileObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp       *itPtr = arg;
    Ns_TclCallback *cbPtr;
    char           *url;
    Tcl_Obj        *scriptObj;
    int             remain = 0, noinherit = 0;
    unsigned int    flags = 0U;
    
    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,   &noinherit, INT2PTR(1)},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",        Ns_ObjvString, &url,       NULL},
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?args",      Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit) { flags |= NS_OP_NOINHERIT;}

    cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclUrl2FileProc, 
			      scriptObj, remain, objv + (objc - remain));
    Ns_RegisterUrl2FileProc(itPtr->servPtr->server, url,
                            NsTclUrl2FileProc, Ns_TclFreeCallback, cbPtr, flags);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUnRegisterUrl2FileObjCmd --
 *
 *      Implements ns_unregister_url2file as obj command. 
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
NsTclUnRegisterUrl2FileObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp     *itPtr = arg;
    CONST char   *url = NULL;
    int           noinherit = 0, recurse = 0;
    unsigned int  flags = 0U;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &noinherit, INT2PTR(1)},
        {"-recurse",   Ns_ObjvBool,  &recurse,   INT2PTR(1)},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",      Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit) { flags |= NS_OP_NOINHERIT;}
    if (recurse)   { flags |= NS_OP_RECURSE;}

    Ns_UnRegisterUrl2FileProc(itPtr->servPtr->server, url, flags);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterFastUrl2FileObjCmd --
 *
 *      Implements ns_register_fasturl2file.  Register the default fast
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
NsTclRegisterFastUrl2FileObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp     *itPtr = arg;
    CONST char   *url = NULL, *basepath = NULL;
    int           noinherit = 0;
    unsigned int  flags = 0U;

    Ns_ObjvSpec opts[] = {
	{"-noinherit", Ns_ObjvBool,  &noinherit, INT2PTR(1)},
        {"--",         Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",        Ns_ObjvString, &url,      NULL},
        {"?basepath",  Ns_ObjvString, &basepath, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (noinherit) { flags |= NS_OP_NOINHERIT;}

    if (basepath == NULL) {
        Ns_RegisterUrl2FileProc(itPtr->servPtr->server, url,
                                Ns_FastUrl2FileProc, NULL, itPtr->servPtr,
                                flags);
    } else {
        Mount *mPtr;

        mPtr = ns_malloc(sizeof(Mount));
        mPtr->basepath = ns_strdup(basepath);
        mPtr->url = ns_strdup(url);
        mPtr->server = itPtr->servPtr->server;
        Ns_RegisterUrl2FileProc(itPtr->servPtr->server, url,
                                NsMountUrl2FileProc, FreeMount, mPtr, flags);
    }

    return TCL_OK;
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

int
NsTclUrl2FileProc(Ns_DString *dsPtr, CONST char *url, void *arg)
{
    Ns_TclCallback *cbPtr = arg;

    if (Ns_TclEvalCallback(NULL, cbPtr, dsPtr, url, NULL) != TCL_OK) {
        return NS_ERROR;
    }
    return NS_OK;
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

int
NsMountUrl2FileProc(Ns_DString *dsPtr, CONST char *url, void *arg)
{
    Mount      *mPtr = arg;
    CONST char *u;

    u = mPtr->url;
    while (*u != '\0' && *url != '\0' && *u == *url) {
        ++u; ++url;
    }
    if (Ns_PathIsAbsolute(mPtr->basepath)) {
        Ns_MakePath(dsPtr, mPtr->basepath, url, NULL);
        return NS_OK;
    }
    fprintf(stderr, "NsMountUrl2FileProc base <%s> url <%s>\n",  mPtr->basepath, url);
    if (Ns_PagePath(dsPtr, mPtr->server, mPtr->basepath, url, NULL) == NULL) {
      fprintf(stderr, "NsMountUrl2FileProc base <%s> url <%s> => NOT FOUND\n",  mPtr->basepath, url);
        return NS_ERROR;
    }
    fprintf(stderr, "NsMountUrl2FileProc base <%s> url <%s> => FOUND\n",  mPtr->basepath, url);
    return NS_OK;
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
NsMountUrl2FileArgProc(Tcl_DString *dsPtr, void *arg)
{
    Mount *mPtr = arg;

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
NsGetUrl2FileProcs(Ns_DString *dsPtr, CONST char *server)
{
    Ns_MutexLock(&ulock);
    Ns_UrlSpecificWalk(uid, server, WalkCallback, dsPtr);
    Ns_MutexUnlock(&ulock);
}

static void
WalkCallback(Ns_DString *dsPtr, void *arg)
{
    Url2File *u2fPtr = arg;

    Ns_GetProcInfo(dsPtr, (Ns_Callback *)u2fPtr->proc, u2fPtr->arg);
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
