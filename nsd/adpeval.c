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
 * adpeval.c --
 *
 *      ADP string and file eval.
 */

#include "nsd.h"

/*
 * The following structure defines a cached ADP page result.  A cached
 * object is created by executing the non-cached code and saving the
 * resulting output which may include embedded non-cached components
 * (see NsTclAdpIncludeObjCmd for details).
 */

typedef struct AdpCache {
    int        refcnt;  /* Current interps using cached results. */
    Ns_Time    expires; /* Expiration time of cached results. */
    AdpCode    code;    /* ADP code for cached result. */
} AdpCache;

/*
 * The following structure defines a shared page in the ADP cache.  The
 * size of the object is extended to include the filename bytes.
 */

typedef struct Page {
    NsServer      *servPtr;  /* Page server context (reg tags, etc.) */
    Tcl_HashEntry *hPtr;     /* Entry in shared table of all pages. */
    time_t         mtime;    /* Original modify time of file. */
    off_t          size;     /* Original size of file. */
    dev_t          dev;      /* Device and inode to try to catch modifications ... */
    ino_t          ino;      /* ...below the mtime granularity. */
    unsigned int   flags;    /* Flags used on last compile, e.g., SAFE. */
    int            refcnt;   /* Refcnt of current interps using page. */
    int            evals;    /* Count of page evaluations. */
    int            locked;   /* Page locked for cache update. */
    int            cacheGen; /* Cache generation id. */
    AdpCache      *cachePtr; /* Cached output. */
    AdpCode        code;     /* ADP code blocks. */
} Page;

/*
 * The following structure holds per-interp script byte codes.  The
 * size of the object is extended based on the number of script objects.
 */

typedef struct Objs {
    int      nobjs;         /* Number of scripts objects. */
    Tcl_Obj *objs[1];       /* Scripts to be compiled and reused. */
} Objs;

/*
 * The following structure defines a per-interp page entry with
 * a pointer to the shared Page and private Objs for cached and
 * non-cached page results.
 */

typedef struct InterpPage {
    Page     *pagePtr;      /* Pointer to shared page text. */
    Objs     *objs;         /* Non-cache ADP code script. */
    int       cacheGen;     /* Cache generation id. */
    Objs     *cacheObjs;    /* Cache results ADP code scripts. */
} InterpPage;

/*
 * Local functions defined in this file.
 */

static Page *ParseFile(const NsInterp *itPtr, const char *file, struct stat *stPtr, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int AdpEval(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *resvar)
    NS_GNUC_NONNULL(1);

static int AdpExec(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *file,
                   const AdpCode *codePtr, Objs *objsPtr, Tcl_DString *outputPtr,
                   const struct stat *stPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(7);

static int AdpSource(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *file,
                     const Ns_Time *expiresPtr, Tcl_DString *outputPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(6);

static int AdpDebug(const NsInterp *itPtr, const char *ptr, int len, int nscript)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void DecrCache(AdpCache *cachePtr)
    NS_GNUC_NONNULL(1);

static Objs *AllocObjs(int nobjs);

static void FreeObjs(Objs *objsPtr)
    NS_GNUC_NONNULL(1);

static void AdpTrace(const NsInterp *itPtr, const char *ptr, int len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_Callback FreeInterpPage;
static Ns_ServerInitProc ConfigServerAdp;


/*
 *----------------------------------------------------------------------
 *
 * NsConfigAdp --
 *
 *      Initialize and configure the ADP subsystem.
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
NsConfigAdp(void)
{
    NsRegisterServerInit(ConfigServerAdp);
}

static Ns_ReturnCode
ConfigServerAdp(const char *server)
{
    NsServer   *servPtr = NsGetServer(server); 
    const char *path;

    path = Ns_ConfigGetPath(server, NULL, "adp", NULL);

    /*
     * Initialize the page and tag tables and locks.
     */

    Tcl_InitHashTable(&servPtr->adp.pages, TCL_STRING_KEYS);
    Ns_MutexInit(&servPtr->adp.pagelock);
    Ns_CondInit(&servPtr->adp.pagecond);
    Ns_MutexSetName2(&servPtr->adp.pagelock, "nsadp:pages", server);
    Tcl_InitHashTable(&servPtr->adp.tags, TCL_STRING_KEYS);
    Ns_RWLockInit(&servPtr->adp.taglock);

    /*
     * Initialise various ADP options.
     */

    servPtr->adp.errorpage = Ns_ConfigString(path, "errorpage", NULL);
    servPtr->adp.startpage = Ns_ConfigString(path, "startpage", NULL);
    servPtr->adp.debuginit = Ns_ConfigString(path, "debuginit", "ns_adp_debuginit");
    servPtr->adp.tracesize = Ns_ConfigInt(path, "tracesize", 40);
    servPtr->adp.cachesize = (size_t)Ns_ConfigInt(path, "cachesize", 5000 * 1024);
    servPtr->adp.bufsize   = (size_t)Ns_ConfigInt(path, "bufsize",   1 * 1024 * 1000);

    servPtr->adp.flags = 0u;
    (void) Ns_ConfigFlag(path, "cache",        ADP_CACHE,     0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "stream",       ADP_STREAM,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "enableexpire", ADP_EXPIRE,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "enabledebug",  ADP_DEBUG,     0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "safeeval",     ADP_SAFE,      0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "singlescript", ADP_SINGLE,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "trace",        ADP_TRACE,     0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "detailerror",  ADP_DETAIL,    1, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "stricterror",  ADP_STRICT,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "displayerror", ADP_DISPLAY,   0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "trimspace",    ADP_TRIM,      0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(path, "autoabort",    ADP_AUTOABORT, 1, &servPtr->adp.flags);

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpEval, NsAdpSource --
 *
 *      Evaluate an ADP string or file and return the output
 *      as the interp result.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Variable named by resvar, if any, is updated with results of
 *      Tcl interp before being replaced with ADP output.
 *
 *----------------------------------------------------------------------
 */

int
NsAdpEval(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *resvar)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    return AdpEval(itPtr, objc, objv, resvar);
}

int
NsAdpSource(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *resvar)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    itPtr->adp.flags |= ADP_ADPFILE;
    return AdpEval(itPtr, objc, objv, resvar);
}

static int
AdpEval(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *resvar)
{
    Tcl_Interp   *interp;
    AdpCode       code;
    Tcl_DString   output;
    int           result;
    char         *obj0;

    NS_NONNULL_ASSERT(itPtr != NULL);

    interp = itPtr->interp;
    /*
     * If the ADP object is a file, simply source it. Otherwise, parse
     * the script as a temporary ADP code object and execute it directly.
     */

    Tcl_DStringInit(&output);
    obj0 = Tcl_GetString(objv[0]);
    if ((itPtr->adp.flags & ADP_ADPFILE) != 0u) {
        result = AdpSource(itPtr, objc, objv, obj0, NULL, &output);
    } else {
        NsAdpParse(&code, itPtr->servPtr, obj0, itPtr->adp.flags, NULL);
        result = AdpExec(itPtr, objc, objv, NULL, &code, NULL, &output, NULL);
        NsAdpFreeCode(&code);
    }

    /*
     * Set the interp result with the ADP output, saving the last interp
     * result first if requested.
     */

    if (result == TCL_OK) {
	Tcl_Obj *objPtr;

        if (resvar != NULL) {
            objPtr = Tcl_GetObjResult(interp);
            if (Tcl_SetVar2Ex(interp, resvar, NULL, objPtr, TCL_LEAVE_ERR_MSG) == NULL) {
                result = TCL_ERROR;
            }
        }
        if (result == TCL_OK) {
            objPtr = Tcl_NewStringObj(output.string, output.length);
            Tcl_SetObjResult(interp, objPtr);
        }
    }
    Tcl_DStringFree(&output);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpInclude --
 *
 *      Evaluate an ADP file, utilizing per-thread byte-code pages.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Output is either left in current ADP buffer.
 *
 *----------------------------------------------------------------------
 */

int
NsAdpInclude(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *file, const Ns_Time *expiresPtr)
{
    Ns_DString *outputPtr;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(file != NULL);

    /*
     * If an ADP execution is already active, use the current output
     * buffer. Otherwise, use the top-level buffer in the ADP struct.
     */

    if (itPtr->adp.framePtr != NULL) {
        outputPtr = itPtr->adp.framePtr->outputPtr;
    } else {
        outputPtr = &itPtr->adp.output;
    }
    return AdpSource(itPtr, objc, objv, file, expiresPtr, outputPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpInit, NsAdpFree --
 *
 *      Initialize or free the NsInterp ADP data structures.
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
NsAdpInit(NsInterp *itPtr)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    Tcl_DStringInit(&itPtr->adp.output);
    NsAdpReset(itPtr);
}

void
NsAdpFree(NsInterp *itPtr)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    if (itPtr->adp.cache != NULL) {
        Ns_CacheDestroy(itPtr->adp.cache);
	itPtr->adp.cache = NULL;
    }
    Tcl_DStringFree(&itPtr->adp.output);
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpReset --
 *
 *      Reset the NsInterp ADP data structures for the next
 *      execution request.
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
NsAdpReset(NsInterp *itPtr)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    itPtr->adp.exception = ADP_OK;
    itPtr->adp.debugLevel = 0;
    itPtr->adp.debugInit = 0;
    itPtr->adp.debugFile = NULL;
    itPtr->adp.chan = NULL;
    itPtr->adp.conn = NULL;
    if (itPtr->servPtr != NULL) {
        itPtr->adp.bufsize = itPtr->servPtr->adp.bufsize;
        itPtr->adp.flags = itPtr->servPtr->adp.flags;
    } else {
        itPtr->adp.bufsize = 1024u * 1000u;
        itPtr->adp.flags = 0u;
    }
    Tcl_DStringTrunc(&itPtr->adp.output, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * AdpSource --
 *
 *      Execute ADP code in a file with results returned in given
 *      dstring.
 *
 * Results:
 *      TCL_ERROR if the file could not be parsed, result of
 *      AdpExec otherwise.
 *
 * Side effects:
 *      Page text and ADP code results may be cached up to given time
 *      limit, if any.
 *
 *----------------------------------------------------------------------
 */

static int
AdpSource(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *file,
          const Ns_Time *expiresPtr, Tcl_DString *outputPtr)
{
    NsServer       *servPtr;
    Tcl_Interp     *interp;
    Tcl_HashEntry  *hPtr;
    struct stat     st;
    Ns_DString      tmp, path;
    InterpPage     *ipagePtr;
    Page           *pagePtr;
    AdpCache       *cachePtr;
    Ns_Time         now;
    int             isNew;
    const char     *p;
    int             result;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(file != NULL);
    NS_NONNULL_ASSERT(outputPtr != NULL);

    servPtr = itPtr->servPtr;
    interp = itPtr->interp;

    ipagePtr = NULL;
    pagePtr = NULL;
    result = TCL_ERROR;   /* assume error until accomplished success */
    Ns_DStringInit(&tmp);
    Ns_DStringInit(&path);

    /*
     * Construct the full, normalized path to the ADP file.
     */

    if (Ns_PathIsAbsolute(file) == NS_FALSE) {
        if (itPtr->adp.cwd == NULL) {
            file = Ns_PagePath(&tmp, servPtr->server, file, NULL);
        } else {
            file = Ns_MakePath(&tmp, itPtr->adp.cwd, file, NULL);
        }
    }
    file = Ns_NormalizePath(&path, file);
    Ns_DStringTrunc(&tmp, 0);

    /*
     * Check for TclPro debugging.
     */

    if (itPtr->adp.debugLevel > 0) {
        ++itPtr->adp.debugLevel;
    } else if (((itPtr->adp.flags & ADP_DEBUG) != 0u)
                   && itPtr->adp.debugFile != NULL
                   && (p = strrchr(file, INTCHAR('/'))) != NULL
                   && Tcl_StringMatch(p+1, itPtr->adp.debugFile) != 0) {
        const Ns_Set *hdrs;
        const char   *host, *port, *procs;

        hdrs = Ns_ConnGetQuery(itPtr->conn);
        host = Ns_SetIGet(hdrs, "dhost");
        port = Ns_SetIGet(hdrs, "dport");
        procs = Ns_SetIGet(hdrs, "dprocs");
        if (NsAdpDebug(itPtr, host, port, procs) != TCL_OK) {
            (void) Ns_ConnReturnNotice(itPtr->conn, 200, "Debug Init Failed",
                                       Tcl_GetStringResult(interp));
            itPtr->adp.exception = ADP_ABORT;
            goto done;
        }
    }

    if (itPtr->adp.cache == NULL) {
	Ns_DStringPrintf(&tmp, "nsadp:%p", (void *)itPtr);
        itPtr->adp.cache = Ns_CacheCreateSz(tmp.string, TCL_STRING_KEYS,
                               itPtr->servPtr->adp.cachesize, FreeInterpPage);
        Ns_DStringTrunc(&tmp, 0);
    }

    /*
     * Verify the file is an existing, ordinary file and get page code.
     */

    if (stat(file, &st) != 0) {
        Ns_TclPrintfResult(interp, "could not stat \"%s\": %s",
                           file, Tcl_PosixError(interp));
    } else if (!S_ISREG(st.st_mode)) {
        Ns_TclPrintfResult(interp, "not an ordinary file: %s", file);
    } else {
	Ns_Entry *ePtr;

        /*
         * Check for valid code in interp page cache.
         */

        ePtr = Ns_CacheFindEntry(itPtr->adp.cache, file);
        if (ePtr != NULL) {
            ipagePtr = Ns_CacheGetValue(ePtr);
            if (ipagePtr->pagePtr->mtime != st.st_mtime
                    || ipagePtr->pagePtr->size != st.st_size
                    || ipagePtr->pagePtr->dev != st.st_dev
                    || ipagePtr->pagePtr->ino != st.st_ino
                    || ipagePtr->pagePtr->flags != itPtr->adp.flags) {
                Ns_CacheFlushEntry(ePtr);
                ipagePtr = NULL;
            }
        }
        if (ipagePtr == NULL) {

            /*
             * Find or create valid page in server table.
             */

            Ns_MutexLock(&servPtr->adp.pagelock);
            hPtr = Tcl_CreateHashEntry(&servPtr->adp.pages, file, &isNew);
            while (isNew == 0 && (pagePtr = Tcl_GetHashValue(hPtr)) == NULL) {
                /* NB: Wait for other thread to read/parse page. */
                Ns_CondWait(&servPtr->adp.pagecond, &servPtr->adp.pagelock);
                hPtr = Tcl_CreateHashEntry(&servPtr->adp.pages, file, &isNew);
            }
            if (isNew == 0 && (pagePtr->mtime != st.st_mtime
                         || pagePtr->size != st.st_size
                         || pagePtr->dev != st.st_dev
                         || pagePtr->ino != st.st_ino
                         || pagePtr->flags != itPtr->adp.flags)) {
                /* NB: Clear entry to indicate read/parse in progress. */
                Tcl_SetHashValue(hPtr, NULL);
                pagePtr->hPtr = NULL;
                isNew = 1;
            }
            if (isNew != 0) {
                Ns_MutexUnlock(&servPtr->adp.pagelock);
                pagePtr = ParseFile(itPtr, file, &st, itPtr->adp.flags);
                Ns_MutexLock(&servPtr->adp.pagelock);
                if (pagePtr == NULL) {
                    Tcl_DeleteHashEntry(hPtr);
                } else {
                    pagePtr->hPtr = hPtr;
                    Tcl_SetHashValue(hPtr, pagePtr);
                }
                Ns_CondBroadcast(&servPtr->adp.pagecond);
            }
            if (pagePtr != NULL) {
                ++pagePtr->refcnt;
            }
            Ns_MutexUnlock(&servPtr->adp.pagelock);
            if (pagePtr != NULL) {
                ipagePtr = ns_malloc(sizeof(InterpPage));
                ipagePtr->pagePtr = pagePtr;
                ipagePtr->cacheGen = 0;
                ipagePtr->objs = AllocObjs(pagePtr->code.nscripts);
                ipagePtr->cacheObjs = NULL;
                ePtr = Ns_CacheCreateEntry(itPtr->adp.cache, file, &isNew);
                if (isNew == 0) {
                    Ns_CacheUnsetValue(ePtr);
                }
                Ns_CacheSetValueSz(ePtr, ipagePtr,
                                   (size_t) ipagePtr->pagePtr->size);
            }
        }
    }

    /*
     * If valid page was found, evaluate it in a new call frame.
     */

    if (ipagePtr != NULL) {
	const AdpCode *codePtr;
        Objs          *objsPtr;
	int            cacheGen = 0;

        pagePtr = ipagePtr->pagePtr;
        if (expiresPtr == NULL || (itPtr->adp.flags & ADP_CACHE) == 0u) {
            cachePtr = NULL;
        } else {

            Ns_MutexLock(&servPtr->adp.pagelock);

            /*
             * First, wait for an initial cache if already executing.
             */

            while ((cachePtr = pagePtr->cachePtr) == NULL && pagePtr->locked == 1) {
                Ns_CondWait(&servPtr->adp.pagecond, &servPtr->adp.pagelock);
            }

            /*
             * Next, if a cache exists and isn't locked, check expiration.
             */

            if (cachePtr != NULL && pagePtr->locked == 0) {
                Ns_GetTime(&now);
                if (Ns_DiffTime(&cachePtr->expires, &now, NULL) < 0) {
                    pagePtr->locked = 1;
                    cachePtr = NULL;
                }
            }

            /*
             * Create the cached page if necessary.
             */

            if (cachePtr == NULL) {
                Ns_MutexUnlock(&servPtr->adp.pagelock);
                codePtr = &pagePtr->code;
                ++itPtr->adp.refresh;
                result = AdpExec(itPtr, objc, objv, file, codePtr, ipagePtr->objs,
                                 &tmp, &st);
                --itPtr->adp.refresh;

                /*
                 * Check cache flag here one more time as we might clear it
                 * inside the script
                 */

                if (result == TCL_OK && (itPtr->adp.flags & ADP_CACHE) != 0u) {
                    cachePtr = ns_malloc(sizeof(AdpCache));

                    /*
                     * Turn off TCL mode after cached result, in caching mode
                     * we wrap Tcl file into proc 'adp:filename' and return
                     * as result only
                     *      ns_adp_append {<% adp:filename %>}
                     *
                     * The output will be cached as result and every time we call
                     * that Tcl file, cached command will be executed as long as
                     * file is unchanged, if modified then the file will be reloaded,
                     * recompiled into same Tcl proc and cached
                     */

                    NsAdpParse(&cachePtr->code, itPtr->servPtr, tmp.string,
                               itPtr->adp.flags & ~ADP_TCLFILE, file);
                    Ns_GetTime(&cachePtr->expires);
                    Ns_IncrTime(&cachePtr->expires, expiresPtr->sec, expiresPtr->usec);
                    cachePtr->refcnt = 1;
                }
                Ns_DStringTrunc(&tmp, 0);
                Ns_MutexLock(&servPtr->adp.pagelock);
                if (cachePtr != NULL) {
                    if (pagePtr->cachePtr != NULL) {
                        DecrCache(pagePtr->cachePtr);
                    }
                    ++pagePtr->cacheGen;
                    pagePtr->cachePtr = cachePtr;
                }
                pagePtr->locked = 0;
                Ns_CondBroadcast(&servPtr->adp.pagecond);
            }
            cacheGen = pagePtr->cacheGen;
            if (cachePtr != NULL) {
                ++cachePtr->refcnt;
            }
            Ns_MutexUnlock(&servPtr->adp.pagelock);
        }
        if (cachePtr == NULL) {
            codePtr = &pagePtr->code;
            objsPtr = ipagePtr->objs;
        } else {
            codePtr = &cachePtr->code;
            if (ipagePtr->cacheObjs != NULL && cacheGen != ipagePtr->cacheGen) {
                FreeObjs(ipagePtr->cacheObjs);
                ipagePtr->cacheObjs = NULL;
            }
            if (ipagePtr->cacheObjs == NULL) {
                ipagePtr->cacheObjs = AllocObjs(AdpCodeScripts(codePtr));
                ipagePtr->cacheGen = cacheGen;
            }
            objsPtr = ipagePtr->cacheObjs;
        }
        result = AdpExec(itPtr, objc, objv, file, codePtr, objsPtr, outputPtr, &st);
        Ns_MutexLock(&servPtr->adp.pagelock);
        ++ipagePtr->pagePtr->evals;
        if (cachePtr != NULL) {
            DecrCache(cachePtr);
        }
        Ns_MutexUnlock(&servPtr->adp.pagelock);
    }
    if (itPtr->adp.debugLevel > 0) {
        --itPtr->adp.debugLevel;
    }

done:
    Ns_DStringFree(&path);
    Ns_DStringFree(&tmp);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpDebug --
 *
 *      Initialize the debugger by calling the debug init proc with
 *      the hostname and port of the debugger and a pattern of procs
 *      to auto-instrument.
 *
 * Results:
 *      TCL_OK if debugger initialized, TCL_ERROR otherwise.
 *
 * Side effects:
 *      Interp is marked for delete on next deallocation.
 *
 *----------------------------------------------------------------------
 */

int
NsAdpDebug(NsInterp *itPtr, const char *host, const char *port, const char *procs)
{
    Tcl_Interp  *interp;
    Tcl_DString  ds;
    int          code;

    NS_NONNULL_ASSERT(itPtr != NULL);

    interp = itPtr->interp;

    code = TCL_OK;
    if (itPtr->adp.debugInit == 0) {
        itPtr->deleteInterp = NS_TRUE;
        Tcl_DStringInit(&ds);
        Tcl_DStringAppendElement(&ds, itPtr->servPtr->adp.debuginit);
        Tcl_DStringAppendElement(&ds, (procs != NULL) ? procs : "");
        Tcl_DStringAppendElement(&ds, (host  != NULL) ? host : "");
        Tcl_DStringAppendElement(&ds, (port  != NULL) ? port : "");
        code = Tcl_EvalEx(interp, ds.string, ds.length, 0);
        Tcl_DStringFree(&ds);
        if (code != TCL_OK) {
            NsAdpLogError(itPtr);
            return TCL_ERROR;
        }

        /*
         * Link the ADP output buffer result to a global variable
         * which can be monitored with a variable watch.
         */

        if (Tcl_LinkVar(interp, "ns_adp_output",
                        (char *) &itPtr->adp.output.string,
                        TCL_LINK_STRING | TCL_LINK_READ_ONLY) != TCL_OK) {
            NsAdpLogError(itPtr);
        }

        itPtr->adp.debugInit = 1;
        itPtr->adp.debugLevel = 1;
    }
    return code;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpStatsCmd --
 *
 *      Implement the ns_adp_stats command to return stats on cached
 *      ADP pages.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpStatsCmd(ClientData clientData, Tcl_Interp *interp, 
		 int UNUSED(argc), CONST char* UNUSED(argv[]))
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    Ns_DString      ds;
    Tcl_HashSearch  search;
    Tcl_HashEntry  *hPtr;

    Ns_DStringInit(&ds);

    Ns_MutexLock(&servPtr->adp.pagelock);
    hPtr = Tcl_FirstHashEntry(&servPtr->adp.pages, &search);
    while (hPtr != NULL) {
	const Page *pagePtr = Tcl_GetHashValue(hPtr);
        char       *file    = Tcl_GetHashKey(&servPtr->adp.pages, hPtr);

        Ns_DStringPrintf(&ds, "{%s} "
            "{dev %" PRIu64 " ino %" PRIu64 " mtime %" PRIu64 " "
            "refcnt %d evals %d size %" PROTd" blocks %d scripts %d} ",
            file,
            (uint64_t) pagePtr->dev, (uint64_t) pagePtr->ino, (uint64_t) pagePtr->mtime,
            pagePtr->refcnt, pagePtr->evals, pagePtr->size,
            pagePtr->code.nblocks, pagePtr->code.nscripts);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&servPtr->adp.pagelock);

    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseFile --
 *
 *      Read and parse text from a file.  The code is complicated
 *      somewhat to account for changing files.
 *
 * Results:
 *      Pointer to new Page structure or NULL on error.
 *
 * Side effects:
 *      Error message will be left in interp on failure.
 *
 *----------------------------------------------------------------------
 */

static Page *
ParseFile(const NsInterp *itPtr, const char *file, struct stat *stPtr, unsigned int flags)
{
    Tcl_Interp   *interp;
    Tcl_Encoding  encoding;
    Tcl_DString   utf;
    char         *buf;
    int           fd, trys;
    ssize_t       n;
    size_t        size;
    Page         *pagePtr;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(file != NULL);
    NS_NONNULL_ASSERT(stPtr != NULL);

    interp = itPtr->interp;

    fd = ns_open(file, O_RDONLY | O_BINARY, 0);
    if (fd < 0) {
        Ns_TclPrintfResult(interp, "could not open \"%s\": %s",
                           file, Tcl_PosixError(interp));
        return NULL;
    }

    pagePtr = NULL;
    buf = NULL;
    trys = 0;
    do {
        /*
         * fstat the open file to ensure it has not changed or been
         * replaced since the original stat.
         */

        if (fstat(fd, stPtr) != 0) {
            Ns_TclPrintfResult(interp, "could not fstat \"%s\": %s",
                               file, Tcl_PosixError(interp));
            goto done;
        }
        size = (size_t)stPtr->st_size;
        buf = ns_realloc(buf, size + 1u);

        /*
         * Attempt to read +1 byte to catch the file growing.
         */

        n = ns_read(fd, buf, size + 1u);
        if (n < 0) {
            Ns_TclPrintfResult(interp, "could not read \"%s\": %s",
                               file, Tcl_PosixError(interp));
            goto done;
        }
        if ((size_t)n != size) {
            /*
             * File is not expected size, rewind and fstat/read again.
             */

            if (ns_lseek(fd, (off_t) 0, SEEK_SET) != 0) {
                Ns_TclPrintfResult(interp, "could not lseek \"%s\": %s",
                                   file, Tcl_PosixError(interp));
                goto done;
            }
            Ns_ThreadYield();
        }
    } while ((size_t)n != size && ++trys < 10);

    if ((size_t)n != size) {
        Ns_TclPrintfResult(interp, "inconsistent file: %s", file);
    } else {
        char *page;

        buf[n] = '\0';
        Tcl_DStringInit(&utf);
        encoding = Ns_GetFileEncoding(file);
        if (encoding == NULL) {
            page = buf;
        } else {
            page = Tcl_ExternalToUtfDString(encoding, buf, (int)n, &utf);
        }
        pagePtr = ns_malloc(sizeof(Page));
        pagePtr->servPtr = itPtr->servPtr;
        pagePtr->flags = flags;
        pagePtr->refcnt = 0;
        pagePtr->evals = 0;
        pagePtr->locked = 0;
        pagePtr->cacheGen = 0;
        pagePtr->cachePtr = NULL;
        pagePtr->mtime = stPtr->st_mtime;
        pagePtr->size = stPtr->st_size;
        pagePtr->dev = stPtr->st_dev;
        pagePtr->ino = stPtr->st_ino;
        NsAdpParse(&pagePtr->code, itPtr->servPtr, page, flags, file);
        Tcl_DStringFree(&utf);
    }

done:
    ns_free(buf);
    (void) ns_close(fd);

    return pagePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpLogError --
 *
 *      Log an ADP error, possibly invoking the log handling ADP
 *      file if configured.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on log handler.
 *
 *----------------------------------------------------------------------
 */

void
NsAdpLogError(NsInterp *itPtr)
{
    Tcl_Interp     *interp;
    const Ns_Conn  *conn;
    Ns_DString      ds;
    const AdpFrame *framePtr;
    int             len;
    const char     *err, *adp, *inc, *dot;

    NS_NONNULL_ASSERT(itPtr != NULL);

    interp = itPtr->interp;
    conn = itPtr->conn;

    framePtr = itPtr->adp.framePtr;
    Ns_DStringInit(&ds);

    if (framePtr != NULL) {
        Ns_DStringPrintf(&ds, "\n    at line %d of ",
                         (int)framePtr->line + Tcl_GetErrorLine(interp));
    }
    inc = "";
    while (framePtr != NULL) {
        if (framePtr->file != NULL) {
            Ns_DStringPrintf(&ds, "%sadp file \"%s\"", inc, framePtr->file);
            if (framePtr->ident != NULL) {
                Ns_DStringPrintf(&ds, " {%s}", Tcl_GetString(framePtr->ident));
            }
        } else {
            adp = Tcl_GetStringFromObj(framePtr->objv[0], &len);
            dot = "";
            if (len > 150) {
                len = 150;
                dot = "...";
            }
            while (((unsigned char)adp[len] & 0xC0u) == 0x80u) {
                /* NB: Avoid truncating multi-byte UTF-8 character. */
                len--;
                dot = "...";
            }
            Ns_DStringPrintf(&ds, "%sadp script:\n\"%.*s%s\"",
                             inc, len, adp, dot);
        }
        framePtr = framePtr->prevPtr;
        inc = "\n    included from ";
    }
    if (conn != NULL && (itPtr->adp.flags & ADP_DETAIL) != 0u) {
	size_t i;

        Ns_DStringPrintf(&ds, "\n    while processing connection %s:\n%8s%s",
                         NsConnIdStr(conn), "",
                         conn->request.line);
        for (i = 0u; i < Ns_SetSize(conn->headers); ++i) {
            Ns_DStringPrintf(&ds, "\n        %s: %s",
                             Ns_SetKey(conn->headers, i),
                             Ns_SetValue(conn->headers, i));
        }
    }
    err = Ns_TclLogErrorInfo(interp, ds.string);
    if ((itPtr->adp.flags & ADP_DISPLAY) != 0u) {
        Ns_DStringTrunc(&ds, 0);
        Ns_DStringAppend(&ds, "<br><pre>\n");
        Ns_QuoteHtml(&ds, err);
        Ns_DStringAppend(&ds, "\n<br></pre>\n");
        (void)NsAdpAppend(itPtr, ds.string, ds.length);
    }
    Ns_DStringFree(&ds);
    adp = itPtr->servPtr->adp.errorpage;
    if (adp != NULL && itPtr->adp.errorLevel == 0) {
	Tcl_Obj *objv[2];

        ++itPtr->adp.errorLevel;
        objv[0] = Tcl_NewStringObj(adp, -1);
        Tcl_IncrRefCount(objv[0]);
        objv[1] = Tcl_GetVar2Ex(interp, "errorInfo", NULL, TCL_GLOBAL_ONLY);
        if (objv[1] == NULL) {
            objv[1] = Tcl_GetObjResult(interp);
        }
        (void) NsAdpInclude(itPtr, 2, objv, adp, NULL);
        Tcl_DecrRefCount(objv[0]);
        --itPtr->adp.errorLevel;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AdpExec --
 *
 *      Execute ADP code.
 *
 * Results:
 *      TCL_OK unless there is an ADP error exception, stack overflow,
 *      or script error when the ADP_STRICT option is set.
 *
 * Side Effects:
 *      Depends on page.
 *
 *----------------------------------------------------------------------
 */

static int
AdpExec(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *file,
        const AdpCode *codePtr, Objs *objsPtr, Tcl_DString *outputPtr,
        const struct stat *stPtr)
{
    Tcl_Interp *interp;
    AdpFrame    frame;
    Ns_DString  cwd;
    Tcl_Obj    *objPtr;
    int         nscript, nblocks, result, i;
    const char *ptr, *savecwd;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(codePtr != NULL);
    NS_NONNULL_ASSERT(outputPtr != NULL);

    interp = itPtr->interp;

    /*
     * Setup the new call frame.
     */

    Ns_DStringInit(&cwd);
    frame.file = file;
    frame.objc = (unsigned short)objc;
    frame.objv = (Tcl_Obj **)objv;
    if (stPtr != NULL) {
        frame.size = stPtr->st_size;
        frame.mtime = stPtr->st_mtime;
    } else {
        frame.size = 0;
        frame.mtime = 0;
    }
    frame.outputPtr = outputPtr;
    frame.ident = NULL;
    savecwd = itPtr->adp.cwd;
    if (file != NULL) {
        const char *slash = strrchr(file, INTCHAR('/'));
        if (slash != NULL) {
            Ns_DStringNAppend(&cwd, file, (int)(slash - file));
            itPtr->adp.cwd = cwd.string;
        }
    }
    frame.prevPtr = itPtr->adp.framePtr;
    itPtr->adp.framePtr = &frame;
    itPtr->adp.depth++;

    /*
     * Execute the ADP by copying text blocks directly to the output
     * stream and evaluating script blocks.
     */

    ptr = AdpCodeText(codePtr);
    nblocks = AdpCodeBlocks(codePtr);
    nscript = 0;
    result = TCL_OK;
    for (i = 0; itPtr->adp.exception == ADP_OK && i < nblocks; ++i) {
	int len; 

        frame.line = (unsigned short)AdpCodeLine(codePtr, i);
        len = AdpCodeLen(codePtr, i);
        if ((itPtr->adp.flags & ADP_TRACE) != 0u) {
            AdpTrace(itPtr, ptr, len);
        }
        if (len > 0) {
            result = NsAdpAppend(itPtr, ptr, len);
        } else {
            len = -len;
            if (itPtr->adp.debugLevel > 0) {
                result = AdpDebug(itPtr, ptr, len, nscript);
            } else if (objsPtr == NULL) {
                result = Tcl_EvalEx(interp, ptr, len, 0);
            } else {
                assert(nscript < objsPtr->nobjs);
                objPtr = objsPtr->objs[nscript];
                if (objPtr == NULL) {
                    objPtr = Tcl_NewStringObj(ptr, len);
                    Tcl_IncrRefCount(objPtr);
                    objsPtr->objs[nscript] = objPtr;
                }
                result = Tcl_EvalObjEx(interp, objPtr, 0);
            }
            ++nscript;

            /*
             * Propagate NS_TIMEOUT errors from Tcl code.
             */

            if (result == TCL_ERROR) {
                if (NsTclTimeoutException(interp) == NS_TRUE) {
                    itPtr->adp.exception = ADP_TIMEOUT;
                }
            }
        }

        /*
         * Log an error message and optionally break from this ADP
         * call frame unless the error was generated to signal
         * and ADP exception.
         */

        if (result != TCL_OK && itPtr->adp.exception == ADP_OK) {
            if ((itPtr->adp.flags & ADP_ERRLOGGED) == 0u) {
                NsAdpLogError(itPtr);
            }
            if ((itPtr->adp.flags & ADP_STRICT) != 0u) {
                itPtr->adp.flags |= ADP_ERRLOGGED;
                break;
            }
        }
        ptr += len;
    }

    /*
     * Clear the return exception and reset result.
     */

    switch (itPtr->adp.exception) {
    case ADP_OK:
        break;
    case ADP_RETURN:
        itPtr->adp.exception = ADP_OK;
        /* FALLTHROUGH */
    case ADP_ABORT:
    case ADP_BREAK:
    case ADP_TIMEOUT:
    default:
        result = TCL_OK;
        break;
    }

    /*
     * Restore the previous call frame.
     */

    itPtr->adp.framePtr = frame.prevPtr;
    itPtr->adp.cwd = savecwd;
    if (frame.ident != NULL) {
        Tcl_DecrRefCount(frame.ident);
    }
    Ns_DStringFree(&cwd);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AdpDebug --
 *
 *      Evaluate an ADP script block with the TclPro debugger.
 *
 * Results:
 *      Depends on script.
 *
 * Side effects:
 *      A unique temp file with header comments and the script is
 *      created and sourced, the effect of which is TclPro will
 *      instrument the code on the fly for single-step debugging.
 *
 *----------------------------------------------------------------------
 */

static int
AdpDebug(const NsInterp *itPtr, const char *ptr, int len, int nscript)
{
    Tcl_Interp *interp;
    int         level;
    const char  *file;
    char        debugfile[255];
    Ns_DString  ds;
    int         result, fd;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(ptr != NULL);

    interp = itPtr->interp;
    level  = itPtr->adp.debugLevel;
    file   = Tcl_GetString(itPtr->adp.framePtr->objv[0]);

    Ns_DStringInit(&ds);
    Ns_DStringPrintf(&ds, "#\n"
                     "# level: %d\n"
                     "# chunk: %d\n"
                     "# file: %s\n"
                     "#\n\n", level, nscript, file);
    Ns_DStringNAppend(&ds, ptr, len);

    snprintf(debugfile, sizeof(debugfile),
             P_tmpdir "/adp%d.%d.XXXXXX",
             level, nscript);
    fd = ns_mkstemp(debugfile);
    if (fd < 0) {
        Tcl_SetResult(interp, "could not create adp debug file", TCL_STATIC);
        result = TCL_ERROR;
    } else {
        if (ns_write(fd, ds.string, (size_t)ds.length) < 0) {
            Ns_TclPrintfResult(interp, "write to \"%s\" failed: %s",
                               debugfile, Tcl_PosixError(interp));
            result = TCL_ERROR;
	} else {
	    Ns_DStringTrunc(&ds, 0);
	    Ns_DStringVarAppend(&ds, "source ", debugfile, NULL);
	    result = Tcl_EvalEx(interp, ds.string, ds.length, 0);
	}
	(void) ns_close(fd);
	unlink(debugfile);
    }
    Ns_DStringFree(&ds);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeInterpPage --
 *
 *      Free a per-interp page cache entry.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
FreeInterpPage(void *arg)
{
    InterpPage *ipagePtr = arg;
    Page       *pagePtr  = ipagePtr->pagePtr;
    NsServer   *servPtr  = pagePtr->servPtr;

    FreeObjs(ipagePtr->objs);
    Ns_MutexLock(&servPtr->adp.pagelock);
    if (--pagePtr->refcnt == 0) {
        if (pagePtr->hPtr != NULL) {
            Tcl_DeleteHashEntry(pagePtr->hPtr);
        }
        if (pagePtr->cachePtr != NULL) {
            FreeObjs(ipagePtr->cacheObjs);
            DecrCache(pagePtr->cachePtr);
        }
        NsAdpFreeCode(&pagePtr->code);
        ns_free(pagePtr);
    }
    Ns_MutexUnlock(&servPtr->adp.pagelock);
    ns_free(ipagePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocObjs --
 *
 *      Allocate new page script objects.
 *
 * Results:
 *      Pointer to new objects.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Objs *
AllocObjs(int nobjs)
{
    Objs *objsPtr;

    objsPtr = ns_calloc(1u, sizeof(Objs) + ((size_t)nobjs * sizeof(Tcl_Obj *)));
    objsPtr->nobjs = nobjs;

    return objsPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeObjs --
 *
 *      Free page objects, decrementing ref counts as needed.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
FreeObjs(Objs *objsPtr)
{
    int i;

    NS_NONNULL_ASSERT(objsPtr != NULL);

    for (i = 0; i < objsPtr->nobjs; ++i) {
        if (objsPtr->objs[i] != NULL) {
            Tcl_DecrRefCount(objsPtr->objs[i]);
        }
    }
    ns_free(objsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * DecrCache --
 *
 *      Decrement ref count of a cache entry, potentially freeing
 *      the cache.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Will free cache on last reference count.
 *
 *----------------------------------------------------------------------
 */

static void
DecrCache(AdpCache *cachePtr)
{
    NS_NONNULL_ASSERT(cachePtr != NULL);

    if (--cachePtr->refcnt == 0) {
        NsAdpFreeCode(&cachePtr->code);
        ns_free(cachePtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AdpTrace --
 *
 *      Trace execution of an ADP page.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Dumps tracing info, possibly truncated, via Ns_Log.
 *
 *----------------------------------------------------------------------
 */

static void
AdpTrace(const NsInterp *itPtr, const char *ptr, int len)
{
    char type;
    
    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(ptr != NULL);

    if (len >= 0) {
        type = 'T';
    } else {
        type = 'S';
        len = -len;
    }
    if (len > itPtr->servPtr->adp.tracesize) {
        len = itPtr->servPtr->adp.tracesize;
    }
    Ns_Log(Notice, "adp[%d%c]: %.*s", itPtr->adp.depth, type, len, ptr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
