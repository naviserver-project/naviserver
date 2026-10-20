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
 * rollfile.c --
 *
 *      Routines to roll files.
 */

#include "nsd.h"

typedef struct File {
    time_t   mtime;
    Tcl_Obj *path;
} File;

/*
 * Expired backups are renamed before logging resumes; only unlink is deferred.
 * The worker owns plain path strings, never Tcl objects or caller Log pointers.
 */
typedef struct PurgePath {
    struct PurgePath *nextPtr;
    char *path;
} PurgePath;

typedef struct PurgeJob {
    struct PurgeJob *nextPtr;
    PurgePath *paths;
    char *filename;
    size_t count;
} PurgeJob;

static Ns_Mutex purgeLock;
static PurgeJob *firstPurge, *lastPurge;
static bool purgeActive, purgeStarted;

static Ns_SchedProc PurgeProc;
static Ns_SchedProc PurgeCleanup;
static bool InitPurgeQueue(void);
static bool SchedulePurge(void);
static void PreservePurgeJobs(PurgeJob *job, const char *reason)
    NS_GNUC_NONNULL(2);
static void QueuePurge(PurgeJob *job)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode RollFile(const char *fileName, TCL_SIZE_T max, PurgeJob *job)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode RollFileFmt(Tcl_Obj *fileObj, const char *rollfmt, TCL_SIZE_T maxbackup, PurgeJob *job)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode PurgeFiles(const char *fileName, TCL_SIZE_T max, PurgeJob *job)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode RetireFile(const char *file, PurgeJob *job)
    NS_GNUC_NONNULL(1);

/*
 * Local functions defined in this file.
 */

static TCL_SIZE_T MatchFiles(Tcl_Obj *pathObj, File **files)
    NS_GNUC_NONNULL(1,2);

static int CmpFile(const void *arg1, const void *arg2)
    NS_GNUC_NONNULL(1,2);

static int Rename(const char *from, const char *to)
    NS_GNUC_NONNULL(1,2);

static int Exists(const char *file)
    NS_GNUC_NONNULL(1);

static int Unlink(const char *file)
    NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 *
 * Ns_RollFile --
 *
 *      Roll the logfile using numbered backups, where filename.000 is the
 *      newest and filename.999 is the highest possible backup number.
 *      This public entry point calls RollFile() with a NULL job, so removal
 *      of excess backups completes synchronously.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on invalid retention or filesystem errors.
 *
 * Side effects:
 *      Renames the active file and shifts existing backup names upward.
 *      May unlink the oldest backup before reusing its numbered name.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_RollFile(const char *fileName, TCL_SIZE_T max)
{
    return RollFile(fileName, max, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * RollFile --
 *
 *      Implement numbered rotation with optional deferred deletion. With a
 *      NULL job, unlink an excess backup immediately. With a non-NULL job,
 *      RetireFile() first moves it to a unique same-directory cleanup path
 *      and appends that path to the caller-owned batch before shifting
 *      backup names. This prevents later deletion of a reused backup name.
 *      The caller retains job ownership and must eventually QueuePurge()
 *      it, including when rotation fails after retiring some files.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on invalid retention or filesystem errors.
 *
 * Side effects:
 *      Renames files and may unlink a backup or append retired paths to job.
 *      Does not enqueue the batch or wait for background deletion.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
RollFile(const char *fileName, TCL_SIZE_T max, PurgeJob *job)
{
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(fileName != NULL);

    if (max <= 0 || max > (TCL_SIZE_T)999) {
        Ns_Log(Error, "rollfile: invalid max parameter '%" PRITcl_Size
               "'; must be > 0 and < 999",
               max);
        status = NS_ERROR;

    } else {
        char  *fromName;
        int    err;
        size_t n = strlen(fileName);

        fromName = ns_malloc_nonzero(n + 5u);
        memcpy(fromName, fileName, n);
        memcpy(fromName + n, ".000", 5);  /* includes NUL */
        err = Exists(fromName);

        if (err > 0) {
            unsigned int num = 0;
            char        *toName = ns_strdup(fromName);

            assert(toName != NULL);

            /*
             * Find the highest version
             */
            do {
                char *dot = strrchr(toName, INTCHAR('.'));

                if (unlikely(dot == NULL)) {
                    Ns_Log(Error, "rollfile: missing version separator in '%s'", toName);
                    err = NS_ERROR;
                    break;
                }

                dot++;
                snprintf(dot, 4u, "%03u", MIN(num, 999u));
                num++;
            } while ((err = Exists(toName)) == 1 && num < (unsigned int)max);

            if (err >= 0) {
                num--; /* After this, num holds the max version found */

                if (err == 1) {
                    err = (RetireFile(toName, job) == NS_OK ? 0 : -1); /* The excessive version */
                }

                /*
                 * Shift existing versions upward, starting at the highest version found.
                 *
                 * "fromName" and "toName" are reusable pathname buffers.  In each iteration,
                 * only the numeric suffix after the final dot is rewritten:
                 *
                 *     fromName = file.NNN
                 *     toName  = file.NNN+1
                 *
                 * Then fromName is renamed to toName, e.g.:
                 *
                 *     file.009 -> file.010
                 *     file.008 -> file.009
                 *     ...
                 *     file.000 -> file.001
                 *
                 * The loop runs backwards to avoid overwriting lower-numbered versions
                 * before they have been moved.
                 */
                while (err == 0 && num > 0u) {
                    char *dot;

                    num--;

                    dot = strrchr(fromName, INTCHAR('.'));
                    if (unlikely(dot == NULL)) {
                        Ns_Log(Error, "rollfile: missing version separator in '%s'", fromName);
                        err = NS_ERROR;
                        break;
                    }
                    dot++;
                    snprintf(dot, 4u, "%03u", MIN(num, 999u));

                    dot = strrchr(toName, INTCHAR('.'));
                    if (unlikely(dot == NULL)) {
                        Ns_Log(Error, "rollfile: missing version separator in '%s'", toName);
                        err = NS_ERROR;
                        break;
                    }
                    dot++;
                    snprintf(dot, 4u, "%03u", MIN(num + 1u, 999u));

                    err = Rename(fromName, toName);
                }
            }
            ns_free_const(toName);
        }

        if (err == 0) {
            err = Exists(fileName);
            if (err > 0) {
                err = Rename(fileName, fromName);
            }
        }

        ns_free(fromName);

        if (err != 0) {
            status = NS_ERROR;
        }
    }
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_RollFileFmt --
 *
 *      Roll the logfile using a timestamp and rollfmt, or numbered backups
 *      when rollfmt is NULL or empty. This public entry point passes a NULL
 *      job to RollFileFmt(), preserving synchronous retention cleanup.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on rotation or retention cleanup errors.
 *
 * Side effects:
 *      Renames the logfile and synchronously removes excess backups.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_RollFileFmt(Tcl_Obj *fileObj, const char *rollfmt, TCL_SIZE_T maxbackup)
{
    return RollFileFmt(fileObj, rollfmt, maxbackup, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * RollFileFmt --
 *
 *      Implement formatted rotation, passing job through to numbered
 *      rotation and retention cleanup. A NULL job requests immediate
 *      deletion; a non-NULL caller-owned job collects retired backup paths
 *      for later QueuePurge(). The batch must be submitted even after a
 *      partial failure. The logfile itself is renamed synchronously.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on rotation or backup retirement errors.
 *
 * Side effects:
 *      Renames files and may unlink excess backups or append paths to job.
 *      Does not reopen the log, enqueue job, or start deletion itself.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
RollFileFmt(Tcl_Obj *fileObj, const char *rollfmt, TCL_SIZE_T maxbackup, PurgeJob *job)
{
    Ns_ReturnCode status;
    const char   *file;

    NS_NONNULL_ASSERT(fileObj != NULL);

    file = Tcl_GetString(fileObj);

    if (rollfmt == NULL || *rollfmt == '\0') {
        status = RollFile(file, maxbackup, job);

    } else {
        time_t           now0, now1 = time(NULL);
        Tcl_DString      ds;
        Tcl_Obj         *newPath;
        struct tm        tm0, tm1;
        const struct tm *ptm0, *ptm1;

        Tcl_DStringInit(&ds);

        /*
         * Rolling happens often at midnight, using often a day
         * precision. When e.g. a scheduled procedure the time when this
         * function is called might be slightly after the scheduled
         * time, which might lead to a day jump. The problem aggravates,
         * when multiple log files are rotated.
         *
         * One approach to address the time variation would be to pass
         * the scheduled timestamp to this function (i.e. not relying on
         * the current time). However, this function might not only be
         * used in the scheduled cases.
         *
         * The approach used below calculates therefore a comparison
         * timestamp 60 seconds before, and in case, this refer to a
         * different day, we assume the mentioned day jump and use the
         * earlier date for calculating the format.
         */
        now0 = now1 - 60;
        ptm0 = ns_localtime_r(&now0, &tm0);
        ptm1 = ns_localtime_r(&now1, &tm1);

        /*
         * In theory, localtime() or localtime_r() can return NULL on
         * invalid input, which won't happen here (famous last words).
         */
        if (ptm0 != NULL && ptm1 != NULL) {
            char timeBuf[512];

#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
            (void) strftime(timeBuf, sizeof(timeBuf)-1u, rollfmt,
                            (ptm0->tm_mday < ptm1->tm_mday) ? ptm0 : ptm1);
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
            Ns_DStringVarAppend(&ds, file, ".", timeBuf, NS_SENTINEL);
        } else {
            Ns_Log(Warning, "RollFileFmt: localtime returned NULL");
            Ns_DStringVarAppend(&ds, file, NS_SENTINEL);
        }

        newPath = Tcl_NewStringObj(ds.string, ds.length);
        Tcl_IncrRefCount(newPath);

        if (Tcl_FSAccess(newPath, F_OK) == 0) {
            status = RollFile(ds.string, maxbackup, job);
        } else if (Tcl_GetErrno() != ENOENT) {
            Ns_Log(Error, "rollfile: access(%s, F_OK) failed: '%s'",
                   ds.string, strerror(Tcl_GetErrno()));
            status = NS_ERROR;
        } else {
            status = NS_OK;
        }
        if (status == NS_OK && Tcl_FSRenameFile(fileObj, newPath) != 0) {
            Ns_Log(Error, "rollfile: rename(%s,%s) failed: '%s'",
                   file, ds.string, strerror(Tcl_GetErrno()));
            status = NS_ERROR;
        }

        Tcl_DecrRefCount(newPath);
        Tcl_DStringFree(&ds);

        if (status == NS_OK) {
            status = PurgeFiles(file, maxbackup, job);
        }
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_RollFileCondFmt --
 *
 *      Function to conditionally roll the file based on the format
 *      string.  This function closes the current logfile, uses
 *      Ns_RollFileFmt() in case, a file with the same name exists, and
 *      (re)opens log logfile again.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:

 *      The logfile is renamed. Expired backups are moved to unique hidden
 *      same-directory paths and unlinked by the serialized purge worker after
 *      reopening the log and re-enabling writes. Caller log locks are never
 *      acquired by the purge worker. Direct roll/purge APIs stay synchronous.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_RollFileCondFmt(Ns_LogCallbackProc openProc, Ns_LogCallbackProc closeProc,
                   const void *arg,
                   const char *filename, const char *rollfmt, TCL_SIZE_T maxbackup)
{
    Ns_ReturnCode status;
    Tcl_DString   errorMsg;
    Ns_Time start, drained, closed, rotated, opened;
    PurgeJob *job = ns_calloc(1u, sizeof(PurgeJob));

    job->filename = ns_strdup(filename);
    Ns_GetTime(&start);

    Tcl_DStringInit(&errorMsg);

    /*
     * We assume, we are already logging to some file.
     */

    NsAsyncWriterQueueDisable(NS_FALSE);
    Ns_GetTime(&drained);

    /*
     * Temporarily close the logfile via the caller-provided callback before
     * rolling it.  The matching openProc callback below reopens and retains
     * the descriptor again.
     */
    status = closeProc(ns_const2voidp(arg));
    Ns_GetTime(&closed);
    if (status == NS_OK) {
        Tcl_Obj      *pathObj;

        pathObj = Tcl_NewStringObj(filename, TCL_INDEX_NONE);
        Tcl_IncrRefCount(pathObj);

        /*
         * If the logfile exists already, roll it.
         */
        if (Tcl_FSAccess(pathObj, F_OK) == 0) {
            /*
             * The current logfile exists.
             */
            status = RollFileFmt(pathObj, rollfmt, maxbackup, job);
            if (status != NS_OK) {
                Ns_DStringPrintf(&errorMsg, "rollfile: rolling logfile failed failed for '%s': %s",
                                 filename, strerror(Tcl_GetErrno()));
            }
        }
        Tcl_DecrRefCount(pathObj);
    } else {
        /*
         * Closing the file did not work. Delay writing the error msg
         * until the logfile is open (we might work on the system log
         * here).
         */
        Ns_DStringPrintf(&errorMsg, "rollfile: closing logfile failed for '%s': %s",
                         filename, strerror(Tcl_GetErrno()));
    }

    /*
     * Re-open the logfile via the caller-provided callback.  The callback
     * installs and retains the resulting descriptor in the corresponding
     * logging subsystem; ownership is not returned to this function.  The
     * descriptor will be released later via the matching closeProc callback.
     */
    Ns_GetTime(&rotated);
    // @infer-ignore PULSE_RESOURCE_LEAK
    status = openProc(ns_const2voidp(arg));
    NsAsyncWriterQueueEnable();
    Ns_GetTime(&opened);

    if (status == NS_OK) {
        if (errorMsg.length > 0) {
            Ns_Log(Warning, "%s", errorMsg.string);
        }
        Ns_Log(Notice, "rollfile: re-opening logfile '%s'", filename);
    } else {
        Ns_Log(Warning, "rollfile: opening logfile '%s' failed: '%s'", filename, strerror(errno));
    }

    {
        Ns_Time drainTime, closeTime, rotateTime, openTime;

        (void)Ns_DiffTime(&drained, &start, &drainTime);
        (void)Ns_DiffTime(&closed, &drained, &closeTime);
        (void)Ns_DiffTime(&rotated, &closed, &rotateTime);
        (void)Ns_DiffTime(&opened, &rotated, &openTime);
        Ns_Log(Notice, "rollfile: timings '%s': drain " NS_TIME_FMT
               "s close " NS_TIME_FMT "s rename-and-retire " NS_TIME_FMT
               "s reopen-and-enable " NS_TIME_FMT "s deferred-files %zu",
               filename, (int64_t)drainTime.sec, drainTime.usec,
               (int64_t)closeTime.sec, closeTime.usec,
               (int64_t)rotateTime.sec, rotateTime.usec,
               (int64_t)openTime.sec, openTime.usec, job->count);
    }
    QueuePurge(job);
    Tcl_DStringFree(&errorMsg);
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_PurgeFiles, Ns_RollFileByDate --
 *
 *      Purge files by date, keeping max files.  The file fileName is
 *      used as a basename to select files to purge. Ns_PurgeFiles()
 *      passes a NULL job to PurgeFiles(), so excess files are unlinked
 *      synchronously before returning.
 *
 *      Ns_RollFileByDate is deprecated and is a poorly named wrapper
 *      for historical reasons (rolling implies rotating filenames).
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      May remove (many) files.
 *
 *----------------------------------------------------------------------
 */
#ifdef NS_WITH_DEPRECATED
Ns_ReturnCode
Ns_RollFileByDate(const char *fileName, TCL_SIZE_T max)
{
    return Ns_PurgeFiles(fileName, max);
}
#endif

Ns_ReturnCode
Ns_PurgeFiles(const char *fileName, TCL_SIZE_T max)
{
    return PurgeFiles(fileName, max, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * PurgeFiles --
 *
 *      Find files matching the fileName basename, sort by descending mtime
 *      and keep at most max files. With a NULL job, unlink excess files
 *      synchronously. With a non-NULL job, move each excess file to a unique
 *      cleanup path via RetireFile() and append it to the caller-owned batch.
 *      The caller must QueuePurge() that batch, even after a partial failure.
 *
 * Results:
 *      NS_OK on success, NS_ERROR on path lookup, unlink or retirement
 *      errors. Successfully retired paths remain in job on partial failure.
 *
 * Side effects:
 *      Scans and sorts directory entries, then removes excess file names.
 *      May allocate retirement entries in job; does not enqueue the batch.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
PurgeFiles(const char *fileName, TCL_SIZE_T max, PurgeJob *job)
{
    Tcl_Obj      *pathObj;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(fileName != NULL);

    pathObj = Tcl_NewStringObj(fileName, TCL_INDEX_NONE);
    Tcl_IncrRefCount(pathObj);

    /*
     * Obtain fully qualified path of the passed filename
     */
    if (Tcl_FSGetNormalizedPath(NULL, pathObj) == NULL) {
        Ns_Log(Error, "rollfile: invalid path '%s'", fileName);
        status = NS_ERROR;

    } else {
        File      *files = NULL;
        TCL_SIZE_T nfiles;

        /*
         * Get all files matching "file*" pattern.
         */
        nfiles = MatchFiles(pathObj, &files);
        if (nfiles == TCL_INDEX_NONE) {
            Ns_Log(Error, "rollfile: failed to match files '%s': %s",
                   fileName, strerror(Tcl_GetErrno()));
            status = NS_ERROR;

        } else if (files != NULL) {
            const File *fiPtr;
            TCL_SIZE_T  ii;

            /*
             * Purge (any) excessive files after sorting them
             * on descening file mtime.
             */

            if (nfiles >= max) {
                qsort(files, (size_t)nfiles, sizeof(File), CmpFile);
                for (ii = max, fiPtr = files + ii; ii < nfiles; ii++, fiPtr++) {
                    if (RetireFile(Tcl_GetString(fiPtr->path), job) != NS_OK) {
                        status = NS_ERROR;
                        break;
                    }
                }
            }

            if (nfiles > 0) {
                for (ii = 0, fiPtr = files; ii < nfiles; ii++, fiPtr++) {
                    Tcl_DecrRefCount(fiPtr->path);
                }
                ns_free(files);
            }
        }
    }
    Tcl_DecrRefCount(pathObj);
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * MatchFiles --
 *
 *      Find plain files in the file's parent directory matching the
 *      "filename*" pattern.
 *
 * Results:
 *      Number of files found, or -1 on error. If any files found,
 *      pointer to allocated array of File structures is left in
 *      the passed "files" argument.
 *
 * Side effects:
 *      Allocates the memory for files array which must be freed
 *      by the caller.
 *
 *----------------------------------------------------------------------
 */

static TCL_SIZE_T
MatchFiles(Tcl_Obj *pathObj, File **files)
{
    Tcl_Obj          *pathElems, *parent, *patternObj, *matched, **matchElems;
    Tcl_GlobTypeData  types;
    Tcl_StatBuf       st;
    int               code;
    TCL_SIZE_T        numElems;
    const char       *pattern;

    NS_NONNULL_ASSERT(pathObj != NULL);
    NS_NONNULL_ASSERT(files != NULL);

    /*
     * Get the parent directory of the passed filename
     */

    pathElems = Tcl_FSSplitPath(pathObj, &numElems);
    parent = Tcl_FSJoinPath(pathElems, numElems - 1);
    Tcl_IncrRefCount(parent);

    /*
     * Construct the glob pattern for lookup.
     */

    if (Tcl_ListObjIndex(NULL, pathElems, numElems - 1, &patternObj) == TCL_OK) {
        Tcl_AppendToObj(patternObj, "*", 1);
        pattern = Tcl_GetString(patternObj);
    } else {
        Ns_Log(Notice, "filename '%s' does not contain a path", Tcl_GetString(pathObj));
        pattern = NS_EMPTY_STRING;
    }

    /*
     * Now, do the match on files only.
     */

    memset(&types, 0, sizeof(Tcl_GlobTypeData));
    types.type = TCL_GLOB_TYPE_FILE;

    matched = Tcl_NewObj();
    Tcl_IncrRefCount(matched);

    code = Tcl_FSMatchInDirectory(NULL, matched, parent, pattern, &types);
    if (code != TCL_OK) {
        numElems = TCL_INDEX_NONE;
    } else {
        /*
         * Construct array of File's to pass to caller
         */

        int result = Tcl_ListObjGetElements(NULL, matched, &numElems, &matchElems);

        if (result == TCL_OK && numElems > 0) {
            File      *fiPtr;
            TCL_SIZE_T ii;

            *files = ns_malloc(sizeof(File) * (size_t)numElems);
            for (ii = 0, fiPtr = *files; ii < numElems; ii++, fiPtr++) {
                if (Tcl_FSStat(matchElems[ii], &st) != 0) {
                    TCL_SIZE_T jj;

                    for (jj = 0, fiPtr = *files; jj < ii; jj++, fiPtr++) {
                        Tcl_DecrRefCount(fiPtr->path);
                    }
                    ns_free(*files);
                    numElems = TCL_INDEX_NONE;
                    break;
                }
                fiPtr->mtime = st.st_mtime;
                fiPtr->path  = matchElems[ii];
                Tcl_IncrRefCount(fiPtr->path);
            }
        }
    }

    Tcl_DecrRefCount(parent);
    Tcl_DecrRefCount(pathElems);
    Tcl_DecrRefCount(matched);

    return numElems;
}


/*
 *----------------------------------------------------------------------
 *
 * CmpFile --
 *
 *      qsort() callback to select oldest file.
 *
 * Results:
 *      Standard qsort() result (-1/0/1)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpFile(const void *arg1, const void *arg2)
{
    int         result;
    const File *f1Ptr = (const File *) arg1;
    const File *f2Ptr = (const File *) arg2;

    if (f1Ptr->mtime < f2Ptr->mtime) {
        result = 1;
    } else if (f1Ptr->mtime > f2Ptr->mtime) {
        result = -1;
    } else {
        result = 0;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Unlink, Rename, Exists --
 *
 *      Simple wrappers used by Ns_RollFile and Ns_PurgeFiles.
 *
 * Results:
 *      System call result (except Exists).
 *
 * Side effects:
 *      May modify filesystem.
 *
 *----------------------------------------------------------------------
 */
static int
Unlink(const char *file)
{
    int err;
    Tcl_Obj *fileObj;

    NS_NONNULL_ASSERT(file != NULL);

    fileObj = Tcl_NewStringObj(file, TCL_INDEX_NONE);
    Tcl_IncrRefCount(fileObj);
    err = Tcl_FSDeleteFile(fileObj);
    if (err != 0) {
        Ns_Log(Error, "rollfile: failed to delete file '%s': '%s'",
               file, strerror(Tcl_GetErrno()));
    }
    Tcl_DecrRefCount(fileObj);

    return err;
}

static int
Rename(const char *from, const char *to)
{
    int err;
    Tcl_Obj *fromObj, *toObj;

    NS_NONNULL_ASSERT(from != NULL);
    NS_NONNULL_ASSERT(to != NULL);

    fromObj = Tcl_NewStringObj(from, TCL_INDEX_NONE);
    Tcl_IncrRefCount(fromObj);

    toObj = Tcl_NewStringObj(to, TCL_INDEX_NONE);
    Tcl_IncrRefCount(toObj);

    err = Tcl_FSRenameFile(fromObj, toObj);

    Tcl_DecrRefCount(fromObj);
    Tcl_DecrRefCount(toObj);
    if (err != 0) {
        Ns_Log(Error, "rollfile: failed to rename file '%s' to '%s': '%s'",
               from, to, strerror(Tcl_GetErrno()));
    }

    return err;
}

static int
Exists(const char *file)
{
    int exists;

    NS_NONNULL_ASSERT(file != NULL);

    if (Tcl_Access(file, F_OK) == 0) {
        exists = 1;
    } else if (Tcl_GetErrno() == ENOENT) {
        exists = 0;
    } else {
        Ns_Log(Error, "rollfile: failed to determine if file '%s' "
               "exists: '%s'", file, strerror(Tcl_GetErrno()));
        exists = -1;
    }

    return exists;
}


/*
 *----------------------------------------------------------------------
 *
 * RetireFile --
 *
 *      Remove an expired backup name without deleting its contents in the
 *      rotation thread. A non-NULL job requests a unique, reserved cleanup
 *      path in the same directory; rename file there and append an owned
 *      path entry to the caller-owned batch. Later rotations can safely
 *      reuse the original name. With a NULL job, simply call Unlink(file).
 *
 * Results:
 *      NS_OK on success, NS_ERROR on unlink or retirement failure.
 *      On successful retirement, job owns the allocated cleanup path.
 *
 * Side effects:
 *      With a non-NULL job, creates an empty reservation, replaces it by
 *      renaming the backup, and increments job->count. Does not copy or
 *      unlink the backup contents or enqueue job. Errors leave the backup
 *      in place when retirement fails. With a NULL job, unlinks file.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
RetireFile(const char *file, PurgeJob *job)
{
    Tcl_Obj      *fileObj, *parts, *parent;
    TCL_SIZE_T    length;
    Tcl_DString   path;
    char         *temporary;
    int           fd;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(file != NULL);

    if (job == NULL) {
        return (Unlink(file) == 0 ? NS_OK : NS_ERROR);
    }

    fileObj = Tcl_NewStringObj(file, -1);
    Tcl_IncrRefCount(fileObj);
    if (Tcl_FSGetNormalizedPath(NULL, fileObj) == NULL) {
        Ns_Log(Error, "rollfile: cannot normalize retired backup '%s'", file);
        Tcl_DecrRefCount(fileObj);
        return NS_ERROR;
    }
    parts = Tcl_FSSplitPath(Tcl_FSGetNormalizedPath(NULL, fileObj), &length);
    parent = Tcl_FSJoinPath(parts, length - 1);
    Tcl_IncrRefCount(parent);
    Tcl_DStringInit(&path);
    Ns_DStringVarAppend(&path, Tcl_GetString(parent), "/.ns-log-purge-XXXXXX", NS_SENTINEL);
    temporary = ns_strdup(path.string);
    fd = ns_mkstemp(temporary);
    if (fd < 0) {
        Ns_Log(Error, "rollfile: cannot reserve cleanup path for '%s': %s", file, strerror(errno));
        result = NS_ERROR;
    } else {
        ns_close(fd);
        result = (Rename(file, temporary) == 0 ? NS_OK : NS_ERROR);
        if (result == NS_OK) {
            PurgePath *entry = ns_malloc(sizeof(PurgePath));

            entry->path = temporary;
            entry->nextPtr = job->paths;
            job->paths = entry;
            ++job->count;
            temporary = NULL;
        } else {
            (void)Unlink(temporary); /* Remove only the empty reservation. */
        }
    }
    ns_free(temporary);
    Tcl_DStringFree(&path);
    Tcl_DecrRefCount(parent);
    Tcl_DecrRefCount(parts);
    Tcl_DecrRefCount(fileObj);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * InitPurgeQueue --
 *
 *      Initialize the process-wide purge queue once via NS_INIT_ONCE and
 *      register its scheduler callback name. Execution and shutdown waiting
 *      are provided by the scheduler; no private thread is created.
 *
 * Results:
 *      NS_TRUE after initialization.
 *
 * Side effects:
 *      Initializes the process-lifetime queue mutex and registers proc info.
 *
 *----------------------------------------------------------------------
 */
static bool
InitPurgeQueue(void)
{
    Ns_MutexInit(&purgeLock);
    Ns_MutexSetName(&purgeLock, "ns:log-purge");
    Ns_RegisterProcInfo((ns_funcptr_t)PurgeProc, "rollfile:purge", NULL);
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * SchedulePurge --
 *
 *      Arm one threaded one-shot drain callback. The caller holds purgeLock
 *      and has established that no callback is active. Keep purgeActive set
 *      until PurgeCleanup() runs, including the gap between PurgeProc()
 *      returning and scheduler cleanup, so new arrivals cannot be stranded.
 *
 * Results:
 *      NS_TRUE if scheduled, NS_FALSE if the scheduler rejects submission.
 *
 * Side effects:
 *      Allocates a scheduler event; sets purgeActive and resets purgeStarted.
 *      Does not create a private worker or wait for file deletion.
 *
 *----------------------------------------------------------------------
 */
static bool
SchedulePurge(void)
{
    Ns_Time interval = {0, 0};

    purgeActive = true;
    purgeStarted = false;
    if (Ns_ScheduleProcEx(PurgeProc, NULL, NS_SCHED_ONCE | NS_SCHED_THREAD,
                          &interval, PurgeCleanup) == (int)NS_ERROR) {
        purgeActive = false;
    }
    return purgeActive;
}


/*
 *----------------------------------------------------------------------
 *
 * PreservePurgeJobs --
 *
 *      Consume a list of batches which cannot be executed. Preserve retired
 *      files on disk and report their paths and the reason for manual
 *      cleanup. The caller has detached these batches under purgeLock and
 *      must not access job after this call.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Logs warnings and frees owned path strings and batch metadata.
 *      Does not acquire purgeLock or unlink any retired file.
 *
 *----------------------------------------------------------------------
 */
static void
PreservePurgeJobs(PurgeJob *job, const char *reason)
{
    NS_NONNULL_ASSERT(reason != NULL);

    while (job != NULL) {
        PurgeJob *nextPtr = job->nextPtr;

        while (job->paths != NULL) {
            PurgePath *entry = job->paths;

            job->paths = entry->nextPtr;
            Ns_Log(Warning, "rollfile: %s preserves retired backup '%s' for manual cleanup", reason, entry->path);
            ns_free(entry->path);
            ns_free(entry);
        }
        ns_free(job->filename);
        ns_free(job);
        job = nextPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * QueuePurge --
 *
 *      Consume a caller-owned batch after reopening the log and re-enabling
 *      the asynchronous writer. Append nonempty batches to the FIFO and arm
 *      a drain callback only if none is scheduled or executing. Empty
 *      batches are freed without scheduling. The caller must not access
 *      job after this call.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Uses purgeLock only for queue state and scheduler submission. Does
 *      not wait for deletion. If shutdown or scheduling failure prevents
 *      execution, preserves retired files and reports their paths.
 *
 *----------------------------------------------------------------------
 */
static void
QueuePurge(PurgeJob *job)
{
    NS_NONNULL_ASSERT(job != NULL);

    if (job->paths == NULL || Ns_InfoShutdownPending()) {
        PreservePurgeJobs(job, "shutdown");
        return;
    }

    NS_INIT_ONCE(InitPurgeQueue);

    Ns_MutexLock(&purgeLock);
    if (lastPurge == NULL) {
        firstPurge = job;
    } else {
        lastPurge->nextPtr = job;
    }
    lastPurge = job;
    job = NULL;
    if (!purgeActive && !SchedulePurge()) {
        job = firstPurge;
        firstPurge = lastPurge = NULL;
    }
    Ns_MutexUnlock(&purgeLock);
    PreservePurgeJobs(job, "scheduler submission failure");
}


/*
 *----------------------------------------------------------------------
 *
 * PurgeProc --
 *
 *      Drain retirement batches in FIFO order on a scheduler worker. Only
 *      owned, uniquely renamed paths are deleted; no directory rescan or
 *      active-log pathname lookup is performed. One callback drains all
 *      available batches, including arrivals during deletion.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Unlinks files without holding purgeLock or any caller log mutex.
 *      Frees consumed batches and reports errors and one timing summary per
 *      batch. Failed deletions leave retired files for manual cleanup. The
 *      scheduler owns worker lifetime and waits for active callbacks at
 *      shutdown. PurgeCleanup() releases or rearms the active callback state.
 *
 *----------------------------------------------------------------------
 */
static void
PurgeProc(void *UNUSED(arg), int UNUSED(id))
{
    Ns_MutexLock(&purgeLock);
    purgeStarted = true;
    while (firstPurge != NULL) {
        PurgeJob *job;
        Ns_Time   start, end, elapsed;
        size_t    failures = 0;

        job = firstPurge;
        firstPurge = job->nextPtr;
        if (firstPurge == NULL) {
            lastPurge = NULL;
        }
        Ns_MutexUnlock(&purgeLock);
        Ns_GetTime(&start);
        while (job->paths != NULL) {
            PurgePath *entry = job->paths;

            job->paths = entry->nextPtr;
            if (Unlink(entry->path) != 0) {
                ++failures;
            }
            ns_free(entry->path);
            ns_free(entry);
        }
        Ns_GetTime(&end);
        (void)Ns_DiffTime(&end, &start, &elapsed);
        Ns_Log(Notice, "rollfile: purge '%s': files %zu failures %zu elapsed " NS_TIME_FMT "s",
               job->filename, job->count, failures, (int64_t)elapsed.sec, elapsed.usec);
        ns_free(job->filename);
        ns_free(job);
        Ns_MutexLock(&purgeLock);
    }
    Ns_MutexUnlock(&purgeLock);
}

/*
 *----------------------------------------------------------------------
 *
 * PurgeCleanup --
 *
 *      Release the completed or cancelled one-shot callback. The scheduler
 *      calls this even when a pending event is discarded at shutdown. If a
 *      completed callback has new arrivals queued since its final empty
 *      check, arm another callback while holding purgeLock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Clears purgeActive. On cancellation before execution, shutdown, or
 *      resubmission failure, detaches and preserves remaining batches for
 *      manual cleanup. Does not unlink files or wait for callbacks; active
 *      callback shutdown waiting belongs to the scheduler.
 *
 *----------------------------------------------------------------------
 */
static void
PurgeCleanup(void *UNUSED(arg), int UNUSED(id))
{
    PurgeJob   *pending = NULL;
    const char *reason = "scheduler callback cancelled";

    Ns_MutexLock(&purgeLock);
    purgeActive = false;
    if (firstPurge != NULL) {
        if (Ns_InfoShutdownPending()) {
            reason = "scheduler shutdown";
        } else if (purgeStarted) {
            if (SchedulePurge()) {
                Ns_MutexUnlock(&purgeLock);
                return;
            }
            reason = "scheduler resubmission failure";
        }
        pending = firstPurge;
        firstPurge = lastPurge = NULL;
    }
    Ns_MutexUnlock(&purgeLock);
    PreservePurgeJobs(pending, reason);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 72
 * indent-tabs-mode: nil
 * End:
 */
