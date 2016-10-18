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

static int MatchFiles(const char *fileName, File **files)
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
 *      Roll the log file. When the log is rolled, it gets renamed to
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
Ns_RollFile(const char *file, int max)
{
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(file != NULL);
    
    if (max < 0 || max > 999) {
        Ns_Log(Error, "rollfile: invalid max parameter '%d'; "
               "must be > 0 and < 999", max);
        status = NS_ERROR;

    } else {
        char *first;
        int   err;

        first = ns_malloc(strlen(file) + 5u);
        sprintf(first, "%s.000", file);
        err = Exists(first);

        if (err > 0) {
            const char *next;
            int         num = 0;

            next = ns_strdup(first);

            /*
             * Find the highest version
             */

            do {
                char *dot = strrchr(next, INTCHAR('.')) + 1;
                sprintf(dot, "%03d", num++);
            } while ((err = Exists(next)) == 1 && num < max);

            num--; /* After this, num holds the max version found */

            if (err == 1) {
                err = Unlink(next); /* The excessive version */
            }

            /*
             * Shift *.010 -> *.011, *:009 -> *.010, etc
             */

            while (err == 0 && num-- > 0) {
                char *dot = strrchr(first, INTCHAR('.')) + 1;
                sprintf(dot, "%03d", num);
                dot = strrchr(next, INTCHAR('.')) + 1;
                sprintf(dot, "%03d", num + 1);
                err = Rename(first, next);
            }
            ns_free((char *)next);
        }

        if (err == 0) {
            err = Exists(file);
            if (err > 0) {
                err = Rename(file, first);
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
 *      Roll the log file either based on a timestamp and a rollfmt, or
 *      based on sequential numbers, when not rollfmt is given.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:

 *      The log file will be renamed, old log files (outside maxbackup)
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
        time_t           now = time(NULL);
        char             timeBuf[512];
        Ns_DString       ds;
        Tcl_Obj         *newPath;
        const struct tm *ptm;

        ptm = ns_localtime(&now);
        (void) strftime(timeBuf, sizeof(timeBuf)-1u, rollfmt, ptm);

        Ns_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, file, ".", timeBuf, NULL);
        newPath = Tcl_NewStringObj(ds.string, -1);
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
Ns_RollFileByDate(const char *file, int max)
{
    return Ns_PurgeFiles(file, max);
}

Ns_ReturnCode
Ns_PurgeFiles(const char *file, int max)
{
    const File   *fiPtr;
    File         *files = NULL;
    int           nfiles;
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(file != NULL);
    
    /*
     * Get all files matching "file*" pattern.
     */

    nfiles = MatchFiles(file, &files);
    if (nfiles == -1) {
        Ns_Log(Error, "rollfile: failed to match files '%s': %s",
               file, strerror(Tcl_GetErrno()));
        status = NS_ERROR;

    } else {
        /*
         * Purge (any) excessive files after sorting them
         * on descening file mtime.
         */

        if (nfiles >= max) {
            int ii;
        
            assert(files != NULL);

            qsort(files, (size_t)nfiles, sizeof(File), CmpFile);
            for (ii = max, fiPtr = files + ii; ii < nfiles; ii++, fiPtr++) {
                if (Unlink(Tcl_GetString(fiPtr->path)) != 0) {
                    goto err;
                }
            }
        }

        status = NS_OK;
    }
    
 err:
    if (nfiles > 0) {
        int ii;

        assert(files != NULL);

        for (ii = 0, fiPtr = files; ii < nfiles; ii++, fiPtr++) {
            Tcl_DecrRefCount(fiPtr->path);
        }
        ns_free(files);
    }

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
MatchFiles(const char *fileName, File **files)
{
    Tcl_Obj          *path, *pathElems, *parent, *patternObj;
    Tcl_Obj          *matched, **matchElems;
    Tcl_GlobTypeData  types;
    Tcl_StatBuf       st;
    int               numElems, code;
    const char       *pattern;

    NS_NONNULL_ASSERT(fileName != NULL);
    NS_NONNULL_ASSERT(files != NULL);
    
    /*
     * Obtain fully qualified path of the passed filename
     */

    path = Tcl_NewStringObj(fileName, -1);
    Tcl_IncrRefCount(path);
    if (Tcl_FSGetNormalizedPath(NULL, path) == NULL) {
        Tcl_DecrRefCount(path);
        return -1;
    }

    /*
     * Get the parent directory of the passed filename
     */

    pathElems = Tcl_FSSplitPath(path, &numElems);
    parent = Tcl_FSJoinPath(pathElems, numElems - 1);
    Tcl_IncrRefCount(parent);

    /*
     * Construct the glob pattern for lookup.
     */

    if (Tcl_ListObjIndex(NULL, pathElems, numElems - 1, &patternObj) == TCL_OK) {
        Tcl_AppendToObj(patternObj, "*", 1);
        pattern = Tcl_GetString(patternObj);
    } else {
        Ns_Log(Notice, "filename '%s' does not contain a path", fileName);
        pattern = "";
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

    Tcl_DecrRefCount(path);
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
