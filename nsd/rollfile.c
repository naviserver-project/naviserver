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
 * Local functions defined in this file.
 */

static int MatchFiles(Tcl_Obj *pathObj, File **files)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int CmpFile(const void *arg1, const void *arg2)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int Rename(const char *from, const char *to)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int Exists(const char *file)
    NS_GNUC_NONNULL(1);

static int Unlink(const char *file)
    NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 *
 * Ns_RollFile --
 *
 *      Roll the logfile. When the log is rolled, it gets renamed to
 *      filename.xyz, where 000 <= xyz <= 999. Older files have higher
 *      numbers.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      If there were files: filename.000, filename.001, filename.002,
 *      the names would end up thusly:
 *          filename.002 => filename.003
 *          filename.001 => filename.002
 *          filename.000 => filename.001
 *      with nothing left named filename.000.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_RollFile(const char *fileName, int max)
{
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(fileName != NULL);

    if (max <= 0 || max > 999) {
        Ns_Log(Error, "rollfile: invalid max parameter '%d'; "
               "must be > 0 and < 999", max);
        status = NS_ERROR;

    } else {
        char  *first;
        int    err;
        size_t bufferSize = strlen(fileName) + 5u;

        first = ns_malloc(bufferSize);
        snprintf(first, bufferSize, "%s.000", fileName);
        err = Exists(first);

        if (err > 0) {
            const char  *next;
            unsigned int num = 0;

            next = ns_strdup(first);

            /*
             * Find the highest version
             */

            do {
                char *dot = strrchr(next, INTCHAR('.')) + 1;
                snprintf(dot, 4u, "%03u", MIN(num, 999u) );
                num ++;
            } while ((err = Exists(next)) == 1 && num < (unsigned int)max);

            num--; /* After this, num holds the max version found */

            if (err == 1) {
                err = Unlink(next); /* The excessive version */
            }

            /*
             * Shift *.010 -> *.011, *:009 -> *.010, etc
             */

            while (err == 0 && num-- > 0) {
                char *dot = strrchr(first, INTCHAR('.')) + 1;
                snprintf(dot, 4u, "%03u", MIN(num, 999u));
                dot = strrchr(next, INTCHAR('.')) + 1;
                snprintf(dot, 4u, "%03u", MIN(num + 1u, 999u));
                err = Rename(first, next);
            }
            ns_free((char *)next);
        }

        if (err == 0) {
            err = Exists(fileName);
            if (err > 0) {
                err = Rename(fileName, first);
            }
        }

        ns_free(first);

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
 *      Roll the logfile either based on a timestamp and a rollfmt, or
 *      based on sequential numbers, when not rollfmt is given.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:

 *      The logfile will be renamed, old logfiles (outside maxbackup)
 *      are deleted.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_RollFileFmt(Tcl_Obj *fileObj, const char *rollfmt, int maxbackup)
{
    Ns_ReturnCode status;
    const char   *file;

    NS_NONNULL_ASSERT(fileObj != NULL);

    file = Tcl_GetString(fileObj);

    if (rollfmt == NULL || *rollfmt == '\0') {
        status = Ns_RollFile(file, maxbackup);

    } else {
        time_t           now0, now1 = time(NULL);
        Ns_DString       ds;
        Tcl_Obj         *newPath;
        struct tm        tm0, tm1, *ptm0, *ptm1;

        Ns_DStringInit(&ds);

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

            (void) strftime(timeBuf, sizeof(timeBuf)-1u, rollfmt,
                            (ptm0->tm_mday < ptm1->tm_mday) ? ptm0 : ptm1);

            Ns_DStringVarAppend(&ds, file, ".", timeBuf, (char *)0L);
        } else {
            Ns_Log(Warning, "RollFileFmt: localtime returned NULL");
            Ns_DStringVarAppend(&ds, file, (char *)0L);
        }

        newPath = Tcl_NewStringObj(ds.string, ds.length);
        Tcl_IncrRefCount(newPath);

        if (Tcl_FSAccess(newPath, F_OK) == 0) {
            status = Ns_RollFile(ds.string, maxbackup);
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
        Ns_DStringFree(&ds);

        if (status == NS_OK) {
            status = Ns_PurgeFiles(file, maxbackup);
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

 *      The logfile will be renamed, old logfiles (outside maxbackup)
 *      are deleted.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_RollFileCondFmt(Ns_LogCallbackProc openProc, Ns_LogCallbackProc closeProc,
                   void *arg,
                   const char *filename, const char *rollfmt, int maxbackup)
{
    Ns_ReturnCode status;
    Tcl_DString   errorMsg;

    Tcl_DStringInit(&errorMsg);

    /*
     * We assume, we are already logging to some file.
     */

    NsAsyncWriterQueueDisable(NS_FALSE);

    /*
     * Close the logfile.
     */
    status = closeProc(arg);
    if (status == NS_OK) {
        Tcl_Obj      *pathObj;

        pathObj = Tcl_NewStringObj(filename, -1);
        Tcl_IncrRefCount(pathObj);

        /*
         * If the logfile exists already, roll it.
         */
        if (Tcl_FSAccess(pathObj, F_OK) == 0) {
            /*
             * The current logfile exists.
             */
            status = Ns_RollFileFmt(pathObj,
                                    rollfmt,
                                    maxbackup);
            if (status != NS_OK) {
                Ns_DStringPrintf(&errorMsg, "log: rolling logfile failed failed for '%s': %s",
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
        Ns_DStringPrintf(&errorMsg, "log: closing logfile failed for '%s': %s",
                         filename, strerror(Tcl_GetErrno()));
    }

    /*
     * Now open the logfile (maybe again).
     */
    status = openProc(arg);
    NsAsyncWriterQueueEnable();

    if (status == NS_OK) {
        if (errorMsg.length > 0) {
            Ns_Log(Warning, "%s", errorMsg.string);
        }
        Ns_Log(Notice, "log: re-opening logfile '%s'", filename);
    } else {
        Ns_Log(Warning, "log: opening logfile failed: '%s'", filename);
    }

    Tcl_DStringFree(&errorMsg);
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_PurgeFiles, Ns_RollFileByDate --
 *
 *      Purge files by date, keeping max files.  The file parameter is
 *      used as a basename to select files to purge.
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

Ns_ReturnCode
Ns_RollFileByDate(const char *fileName, int max)
{
    return Ns_PurgeFiles(fileName, max);
}

Ns_ReturnCode
Ns_PurgeFiles(const char *fileName, int max)
{
    Tcl_Obj      *pathObj;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(fileName != NULL);

    pathObj = Tcl_NewStringObj(fileName, -1);
    Tcl_IncrRefCount(pathObj);

    /*
     * Obtain fully qualified path of the passed filename
     */
    if (Tcl_FSGetNormalizedPath(NULL, pathObj) == NULL) {
        Ns_Log(Error, "rollfile: invalid path '%s'", fileName);
        status = NS_ERROR;

    } else {
        File *files = NULL;
        int   nfiles;

        /*
         * Get all files matching "file*" pattern.
         */
        nfiles = MatchFiles(pathObj, &files);
        if (nfiles == -1) {
            Ns_Log(Error, "rollfile: failed to match files '%s': %s",
                   fileName, strerror(Tcl_GetErrno()));
            status = NS_ERROR;

        } else if (files != NULL) {
            const File *fiPtr;
            int         ii;

            /*
             * Purge (any) excessive files after sorting them
             * on descening file mtime.
             */

            if (nfiles >= max) {
                qsort(files, (size_t)nfiles, sizeof(File), CmpFile);
                for (ii = max, fiPtr = files + ii; ii < nfiles; ii++, fiPtr++) {
                    if (Unlink(Tcl_GetString(fiPtr->path)) != 0) {
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

static int
MatchFiles(Tcl_Obj *pathObj, File **files)
{
    Tcl_Obj          *pathElems, *parent, *patternObj, *matched, **matchElems;
    Tcl_GlobTypeData  types;
    Tcl_StatBuf       st;
    int               numElems, code;
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
        numElems = -1;
    } else {
        /*
         * Construct array of File's to pass to caller
         */

        int result = Tcl_ListObjGetElements(NULL, matched, &numElems, &matchElems);

        if (result == TCL_OK && numElems > 0) {
            File *fiPtr;
            int   ii;

            *files = ns_malloc(sizeof(File) * (size_t)numElems);
            for (ii = 0, fiPtr = *files; ii < numElems; ii++, fiPtr++) {
                if (Tcl_FSStat(matchElems[ii], &st) != 0) {
                    int jj;

                    for (jj = 0, fiPtr = *files; jj < ii; jj++, fiPtr++) {
                        Tcl_DecrRefCount(fiPtr->path);
                    }
                    ns_free(*files);
                    numElems = -1;
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

    fileObj = Tcl_NewStringObj(file, -1);
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

    fromObj = Tcl_NewStringObj(from, -1);
    Tcl_IncrRefCount(fromObj);

    toObj = Tcl_NewStringObj(to, -1);
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
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 72
 * indent-tabs-mode: nil
 * End:
 */
