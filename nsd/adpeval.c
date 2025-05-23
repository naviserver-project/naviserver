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
 * adpeval.c --
 *
 *      ADP string and file eval.
 */

#include "nsd.h"

#define AdpCodeLen(cp,i)    ((cp)->len[(i)])
#define AdpCodeLine(cp,i)   ((cp)->line[(i)])
#define AdpCodeText(cp)     ((cp)->text.string)
#define AdpCodeBlocks(cp)   ((cp)->nblocks)
#define AdpCodeScripts(cp)  ((cp)->nscripts)

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
    int            cacheGen; /* Cache generation id. */
    AdpCache      *cachePtr; /* Cached output. */
    AdpCode        code;     /* ADP code blocks. */
    bool           locked;   /* Page locked for cache update. */
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

static int AdpEval(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *resvar)
    NS_GNUC_NONNULL(1);

static int AdpExec(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *file,
                   const AdpCode *codePtr, Objs *objsPtr, Tcl_DString *outputPtr,
                   const struct stat *stPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(7);

static int AdpSource(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *file,
                     const Ns_Time *expiresPtr, Tcl_DString *outputPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(6);

static int AdpDebug(const NsInterp *itPtr, const char *ptr, TCL_SIZE_T len, int nscript)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void DecrCache(AdpCache *cachePtr)
    NS_GNUC_NONNULL(1);

static Objs *AllocObjs(int nobjs);

static void FreeObjs(Objs *objsPtr)
    NS_GNUC_NONNULL(1);

static void AdpTrace(const NsInterp *itPtr, const char *ptr, TCL_SIZE_T len)
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
    const char *section;

    section = Ns_ConfigSectionPath(NULL, server, NULL, "adp", NS_SENTINEL);

    /*
     * Initialize the page and tag tables and locks.
     */

    Tcl_InitHashTable(&servPtr->adp.pages, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->adp.tags, TCL_STRING_KEYS);

    Ns_CondInit(&servPtr->adp.pagecond);

    Ns_MutexInit(&servPtr->adp.pagelock);
    Ns_MutexSetName2(&servPtr->adp.pagelock, "ns:adp:pages", server);

    Ns_RWLockInit(&servPtr->adp.taglock);
    Ns_RWLockSetName2(&servPtr->adp.taglock, "rw:adp:tags", server);

    /*
     * Initialise various ADP options.
     */

    servPtr->adp.errorpage = ns_strcopy(Ns_ConfigString(section, "errorpage", NULL));
    servPtr->adp.startpage = ns_strcopy(Ns_ConfigString(section, "startpage", NULL));
    servPtr->adp.debuginit = ns_strcopy(Ns_ConfigString(section, "debuginit", "ns_adp_debuginit"));
    servPtr->adp.tracesize = Ns_ConfigInt(section, "tracesize", 40);
    servPtr->adp.cachesize = (size_t)Ns_ConfigMemUnitRange(section, "cachesize", "5MB", 5000 * 1024,
                                                           1000 * 1024, INT_MAX);
    servPtr->adp.bufsize   = (size_t)Ns_ConfigMemUnitRange(section, "bufsize",  "1MB",  1024 * 1000,
                                                           100 * 1024, INT_MAX);
    servPtr->adp.defaultExtension = ns_strcopy(Ns_ConfigString(section, "defaultextension", NULL));

    servPtr->adp.flags = 0u;
    (void) Ns_ConfigFlag(section, "cache",        ADP_CACHE,     0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "stream",       ADP_STREAM,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "enableexpire", ADP_EXPIRE,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "enabledebug",  ADP_DEBUG,     0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "safeeval",     ADP_SAFE,      0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "singlescript", ADP_SINGLE,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "trace",        ADP_TRACE,     0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "detailerror",  ADP_DETAIL,    1, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "stricterror",  ADP_STRICT,    0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "displayerror", ADP_DISPLAY,   0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "trimspace",    ADP_TRIM,      0, &servPtr->adp.flags);
    (void) Ns_ConfigFlag(section, "autoabort",    ADP_AUTOABORT, 1, &servPtr->adp.flags);

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
NsAdpEval(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *resvar)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    return AdpEval(itPtr, objc, objv, resvar);
}

int
NsAdpSource(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *resvar)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    itPtr->adp.flags |= ADP_ADPFILE;
    return AdpEval(itPtr, objc, objv, resvar);
}

static int
AdpEval(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *resvar)
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
NsAdpInclude(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *file, const Ns_Time *expiresPtr)
{
    Tcl_DString *outputPtr;

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
    Tcl_DStringSetLength(&itPtr->adp.output, 0);
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
AdpSource(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *file,
          const Ns_Time *expiresPtr, Tcl_DString *outputPtr)
{
    NsServer       *servPtr;
    Tcl_Interp     *interp;
    Tcl_HashEntry  *hPtr;
    struct stat     st;
    Tcl_DString     tmp, path;
    InterpPage     *ipagePtr = NULL;
    Page           *pagePtr = NULL;
    Ns_Time         now;
    int             isNew;
    const char     *p;
    int             result = TCL_ERROR;   /* assume error until accomplished success */

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(file != NULL);
    NS_NONNULL_ASSERT(outputPtr != NULL);

    servPtr = itPtr->servPtr;
    interp = itPtr->interp;

    Tcl_DStringInit(&tmp);
    Tcl_DStringInit(&path);

    /*
     * Construct the full, normalized path to the ADP file.
     */

    if (Ns_PathIsAbsolute(file) == NS_FALSE) {
        if (itPtr->adp.cwd == NULL) {
            file = Ns_PagePath(&tmp, servPtr->server, file, NS_SENTINEL);
        } else {
            file = Ns_MakePath(&tmp, itPtr->adp.cwd, file, NS_SENTINEL);
        }
    }
    file = Ns_NormalizePath(&path, file);
    Tcl_DStringSetLength(&tmp, 0);

    /*
     * Check for TclPro debugging.
     */

    if (itPtr->adp.debugLevel > 0) {
        ++itPtr->adp.debugLevel;
    } else if ((itPtr->conn != NULL)
               && ((itPtr->adp.flags & ADP_DEBUG) != 0u)
               && itPtr->adp.debugFile != NULL
               && (p = strrchr(file, INTCHAR('/'))) != NULL
               && Tcl_StringMatch(p+1, itPtr->adp.debugFile) != 0) {
        const Ns_Set *query;
        const char   *host, *port, *procs;

        query = Ns_ConnGetQuery(interp, itPtr->conn, NULL, NULL); /* currently ignoring encoding errors */
        host = Ns_SetIGet(query, "dhost");
        port = Ns_SetIGet(query, "dport");
        procs = Ns_SetIGet(query, "dprocs");
        if (NsAdpDebug(itPtr, host, port, procs) != TCL_OK) {
            /*
             * In case the debugger setup is not correct, avoid a call to
             * Ns_ConnReturnNotice() which triggers an ADP page, running most
             * likely again into the same ADP setup error.  Therefore, stick to
             * the lower-level, non-templating variant of the response.
             */
            Tcl_DString ds;

            Tcl_DStringInit(&ds);
            Ns_DStringPrintf(&ds, "TclPro Debug Init Failed: %s", Tcl_GetString(Tcl_GetObjResult(interp)));
            result = Ns_ConnReturnCharData(itPtr->conn, 200, ds.string, ds.length, "text/plain");
            Tcl_DStringFree(&ds);

            itPtr->adp.exception = ADP_ABORT;
            goto done;
        }
    }

    if (itPtr->adp.cache == NULL) {
        Ns_DStringPrintf(&tmp, "nsadp:%p", (void *)itPtr);
        itPtr->adp.cache = Ns_CacheCreateSz(tmp.string, TCL_STRING_KEYS,
                               itPtr->servPtr->adp.cachesize, FreeInterpPage);
        Tcl_DStringSetLength(&tmp, 0);
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
                Ns_Log(Debug, "AdpSource calls ParseFile with flags %.8x", itPtr->adp.flags);
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
        AdpCache      *cachePtr;

        pagePtr = ipagePtr->pagePtr;
        if (expiresPtr == NULL || (itPtr->adp.flags & ADP_CACHE) == 0u) {
            cachePtr = NULL;
        } else {

            Ns_MutexLock(&servPtr->adp.pagelock);

            /*
             * First, wait for an initial cache if already executing.
             */

            while ((cachePtr = pagePtr->cachePtr) == NULL && pagePtr->locked) {
                Ns_CondWait(&servPtr->adp.pagecond, &servPtr->adp.pagelock);
            }

            /*
             * Next, if a cache exists and isn't locked, check expiration.
             */

            if (cachePtr != NULL && ! pagePtr->locked) {
                Ns_GetTime(&now);
                if (Ns_DiffTime(&cachePtr->expires, &now, NULL) < 0) {
                    pagePtr->locked = NS_TRUE;
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
                     * Turn off Tcl mode after cached result, in caching mode
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
                Tcl_DStringSetLength(&tmp, 0);
                Ns_MutexLock(&servPtr->adp.pagelock);
                if (cachePtr != NULL) {
                    if (pagePtr->cachePtr != NULL) {
                        DecrCache(pagePtr->cachePtr);
                    }
                    ++pagePtr->cacheGen;
                    pagePtr->cachePtr = cachePtr;
                }
                pagePtr->locked = NS_FALSE;
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

        Ns_Log(Debug, "AdpSource calls AdpExec nblocks %d with objc %ld codePtr->text <%s>",
               codePtr->nblocks, (long)objc, codePtr->text.string);

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
    Tcl_DStringFree(&path);
    Tcl_DStringFree(&tmp);

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
NsAdpDebug(NsInterp *itPtr, const char *debugHost, const char *debugPort, const char *debugProcs)
{
    Tcl_Interp  *interp;
    int          result;

    NS_NONNULL_ASSERT(itPtr != NULL);

    interp = itPtr->interp;
    result = TCL_OK;

    if (itPtr->adp.debugInit == 0) {
        Tcl_DString  ds, scratch;

        Tcl_DStringInit(&scratch);
        if (debugHost == NULL) {
            if (itPtr->conn == NULL) {
                Ns_Log(Warning, "NsAdpDebug no connection available,"
                       "please provide debug host explicitly");
                debugHost = "localhost";
            } else {
                const char *errorMsg;
                Ns_URL      url;

                /*
                 * Using Ns_ConnLocationAppend() might look like an overkill,
                 * since it returns more information the necessary. However,
                 * it deals with host header field validation, virtual
                 * hosting, default value management, etc.
                 */
                Ns_ConnLocationAppend(itPtr->conn, &scratch);
                Ns_ParseUrl(scratch.string, NS_FALSE, &url, &errorMsg);
                debugHost = url.host;
            }
        }

        itPtr->deleteInterp = NS_TRUE;
        Tcl_DStringInit(&ds);
        Tcl_DStringAppendElement(&ds, itPtr->servPtr->adp.debuginit);
        Tcl_DStringAppendElement(&ds, (debugProcs != NULL) ? debugProcs : NS_EMPTY_STRING);
        Tcl_DStringAppendElement(&ds, debugHost);
        Tcl_DStringAppendElement(&ds, (debugPort != NULL && *debugPort == '\0') ? debugPort : "2576");
        result = Tcl_EvalEx(interp, ds.string, ds.length, 0);
        Tcl_DStringFree(&ds);
        Tcl_DStringFree(&scratch);
        if (result != TCL_OK) {
            NsAdpLogError(itPtr);
            result = TCL_ERROR;

        } else {
            /*
             * Link the ADP output buffer result to a global variable
             * which can be monitored with a variable watch.
             */
            if (Tcl_LinkVar(interp, "ns_adp_output",
                            (char *) &itPtr->adp.output.string,
                            TCL_LINK_STRING | TCL_LINK_READ_ONLY) != TCL_OK) {
                Ns_Log(Notice,"NsAdpDebug provides linkage to ns_adp_output variable, calling NsAdpLogError()");
                NsAdpLogError(itPtr);
            }

            itPtr->adp.debugInit = 1;
            itPtr->adp.debugLevel = 1;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpStatsObjCmd --
 *
 *      Implements "ns_adp_stats". This command returns statistics about
 *      cached ADP pages.
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
NsTclAdpStatsObjCmd(ClientData clientData, Tcl_Interp *interp,
                    TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        Tcl_DString     ds;
        Tcl_HashSearch  search;
        Tcl_HashEntry  *hPtr;

        Tcl_DStringInit(&ds);
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
    }
    return result;
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
    char         *buf = NULL;
    int           fd, tries = 0;
    size_t        size = 0u;
    ssize_t       n;
    Page         *pagePtr = NULL;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(file != NULL);
    NS_NONNULL_ASSERT(stPtr != NULL);

    interp = itPtr->interp;

    fd = ns_open(file, O_RDONLY | O_BINARY | O_CLOEXEC, 0);
    if (fd < 0) {
        Ns_TclPrintfResult(interp, "could not open \"%s\": %s",
                           file, Tcl_PosixError(interp));
        return NULL;
    }

    do {
        /*
         * fstat() the open file to ensure it has not changed or been replaced
         * since the original stat().
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
    } while ((size_t)n != size && ++tries < 10);

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
            page = Tcl_ExternalToUtfDString(encoding, buf, (TCL_SIZE_T)n, &utf);
        }
        pagePtr = ns_malloc(sizeof(Page));
        pagePtr->servPtr = itPtr->servPtr;
        pagePtr->flags = flags;
        pagePtr->refcnt = 0;
        pagePtr->evals = 0;
        pagePtr->locked = NS_FALSE;
        pagePtr->cacheGen = 0;
        pagePtr->cachePtr = NULL;
        pagePtr->mtime = stPtr->st_mtime;
        pagePtr->size = stPtr->st_size;
        pagePtr->dev = stPtr->st_dev;
        pagePtr->ino = stPtr->st_ino;
        Ns_Log(Debug, "ParseFile calls NsAdpParse with flags %.8x", flags);
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
    Tcl_DString     ds;
    const AdpFrame *framePtr;
    TCL_SIZE_T      len;
    const char     *err, *adp, *inc, *dot;

    NS_NONNULL_ASSERT(itPtr != NULL);

    interp = itPtr->interp;
    conn = itPtr->conn;

    framePtr = itPtr->adp.framePtr;
    Tcl_DStringInit(&ds);

    if (framePtr != NULL) {
        Ns_DStringPrintf(&ds, "\n    at line %d of ",
                         (int)framePtr->line + Tcl_GetErrorLine(interp));
    }
    inc = NS_EMPTY_STRING;
    while (framePtr != NULL) {
        if (framePtr->file != NULL) {
            Ns_DStringPrintf(&ds, "%sadp file \"%s\"", inc, framePtr->file);
            if (framePtr->ident != NULL) {
                Ns_DStringPrintf(&ds, " {%s}", Tcl_GetString(framePtr->ident));
            }
        } else {
            adp = Tcl_GetStringFromObj(framePtr->objv[0], &len);
            dot = NS_EMPTY_STRING;
            if (len > 150) {
                len = 150;
                dot = "...";
            }
            while (((unsigned char)adp[len] & 0xC0u) == 0x80u) {
                /*
                 * Avoid truncating multi-byte UTF-8 character.
                 */
                len--;
                dot = "...";
            }
            Ns_DStringPrintf(&ds, "%sadp script:\n\"%.*s%s\"",
                             inc, (int)len, adp, dot);
        }
        framePtr = framePtr->prevPtr;
        inc = "\n    included from ";
    }
    if (conn != NULL && (itPtr->adp.flags & ADP_DETAIL) != 0u) {
        size_t i;

        Ns_DStringPrintf(&ds, "\n    while processing connection %s:\n%8s%s",
                         NsConnIdStr(conn), NS_EMPTY_STRING,
                         conn->request.line);
        for (i = 0u; i < Ns_SetSize(conn->headers); ++i) {
            Ns_DStringPrintf(&ds, "\n        %s: %s",
                             Ns_SetKey(conn->headers, i),
                             Ns_SetValue(conn->headers, i));
        }
    }
    err = Ns_TclLogErrorInfo(interp, ds.string);
    if ((itPtr->adp.flags & ADP_DISPLAY) != 0u) {
        Tcl_DStringSetLength(&ds, 0);
        Tcl_DStringAppend(&ds, "<br><pre>\n", 10);
        Ns_QuoteHtml(&ds, err);
        Tcl_DStringAppend(&ds, "\n<br></pre>\n", 12);
        (void)NsAdpAppend(itPtr, ds.string, ds.length);
    }
    Tcl_DStringFree(&ds);
    adp = itPtr->servPtr->adp.errorpage;
    if (adp != NULL && itPtr->adp.errorLevel == 0) {
        Tcl_Obj *objv[2];

        ++itPtr->adp.errorLevel;
        objv[0] = Tcl_NewStringObj(adp, TCL_INDEX_NONE);
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
AdpExec(NsInterp *itPtr, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *file,
        const AdpCode *codePtr, Objs *objsPtr, Tcl_DString *outputPtr,
        const struct stat *stPtr)
{
    Tcl_Interp *interp;
    AdpFrame    frame;
    Tcl_DString cwd;
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

    Tcl_DStringInit(&cwd);
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
            Tcl_DStringAppend(&cwd, file, (TCL_SIZE_T)(slash - file));
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
        TCL_SIZE_T len;

        frame.line = (unsigned short)AdpCodeLine(codePtr, i);
        len = AdpCodeLen(codePtr, i);
        if ((itPtr->adp.flags & ADP_TRACE) != 0u) {
            AdpTrace(itPtr, ptr, len);
        }
        if (len > 0) {
            result = NsAdpAppend(itPtr, ptr, (TCL_SIZE_T)len);
        } else {
            len = -len;
            if (itPtr->adp.debugLevel > 0) {
                result = AdpDebug(itPtr, ptr, (TCL_SIZE_T)len, nscript);

            } else if (objsPtr == NULL) {
                result = Tcl_EvalEx(interp, ptr, (TCL_SIZE_T)len, 0);
            } else {
                assert(nscript < objsPtr->nobjs);
                objPtr = objsPtr->objs[nscript];
                if (objPtr == NULL) {
                    objPtr = Tcl_NewStringObj(ptr, (TCL_SIZE_T)len);
                    Tcl_IncrRefCount(objPtr);
                    objsPtr->objs[nscript] = objPtr;
                }
                Ns_Log(Debug, "AdpExec calls Tcl_EvalObjEx with cmd <%s>", Tcl_GetString(objPtr));
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
        NS_FALL_THROUGH; /* fall through */
    case ADP_ABORT:
        NS_FALL_THROUGH; /* fall through */
    case ADP_BREAK:
        NS_FALL_THROUGH; /* fall through */
    case ADP_TIMEOUT:
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
    Tcl_DStringFree(&cwd);

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
AdpDebug(const NsInterp *itPtr, const char *ptr, TCL_SIZE_T len, int nscript)
{
    Tcl_Interp *interp;
    int         level;
    const char  *file;
    char        debugfile[255];
    Tcl_DString ds;
    int         result, fd;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(ptr != NULL);

    interp = itPtr->interp;
    level  = itPtr->adp.debugLevel;
    file   = Tcl_GetString(itPtr->adp.framePtr->objv[0]);

    Tcl_DStringInit(&ds);
    Ns_DStringPrintf(&ds, "#\n"
                     "# level: %d\n"
                     "# chunk: %d\n"
                     "# file: %s\n"
                     "#\n\n", level, nscript, file);
    Tcl_DStringAppend(&ds, ptr, len);

    snprintf(debugfile, sizeof(debugfile),
             P_tmpdir "/adp%d.%d.XXXXXX",
             level, nscript);
    fd = ns_mkstemp(debugfile);
    if (fd < 0) {
        Ns_TclPrintfResult(interp, "could not create ADP debug file");
        result = TCL_ERROR;
    } else {
        if (ns_write(fd, ds.string, (size_t)ds.length) < 0) {
            Ns_TclPrintfResult(interp, "write to \"%s\" failed: %s",
                               debugfile, Tcl_PosixError(interp));
            result = TCL_ERROR;
        } else {
            Tcl_DStringSetLength(&ds, 0);
            Ns_DStringVarAppend(&ds, "source ", debugfile, NS_SENTINEL);
            result = Tcl_EvalEx(interp, ds.string, ds.length, 0);
        }
        (void) ns_close(fd);
        unlink(debugfile);
    }
    Tcl_DStringFree(&ds);

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
AdpTrace(const NsInterp *itPtr, const char *ptr, TCL_SIZE_T len)
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
    Ns_Log(Notice, "adp[%d%c]: %.*s", itPtr->adp.depth, type, (int)len, ptr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
