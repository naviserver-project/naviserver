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

static int MatchFiles(CONST char *file, File **files);
static int CmpFile(const void *p1, const void *p2);
static int Rename(CONST char *from, CONST char *to);
static int Exists(CONST char *file);
static int Unlink(CONST char *file);


/*
 *----------------------------------------------------------------------
 *
 * Ns_RollFile --
 *
 *      Roll the log file. When the log is rolled, it gets renamed to
 *      filename.xyz, where 000 <= xyz <= 999. Older files have
 *      higher numbers.
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

int
Ns_RollFile(CONST char *file, int max)
{
    char *first, *next, *dot;
    int   err;

    if (max < 0 || max > 999) {
        Ns_Log(Error, "rollfile: invalid max parameter '%d'; "
               "must be > 0 and < 999", max);
        return NS_ERROR;
    }

    first = ns_malloc(strlen(file) + 5);
    sprintf(first, "%s.000", file);
    err = Exists(first);

    if (err > 0) {
        int num = 0;

        next = ns_strdup(first);

        /*
         * Find the highest version
         */

        do {
            dot = strrchr(next, '.') + 1;
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
            dot = strrchr(first, '.') + 1;
            sprintf(dot, "%03d", num);
            dot = strrchr(next, '.') + 1;
            sprintf(dot, "%03d", num + 1);
            err = Rename(first, next);
        }
        ns_free(next);
    }

    if (err == 0) {
        err = Exists(file);
        if (err > 0) {
            err = Rename(file, first);
        }
    }

    ns_free(first);

    if (err != 0) {
        return NS_ERROR;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_PurgeFiles, Ns_RollFileByDate --
 *
 *      Purge files by date, keeping max files.  The file parameter is
 *      used as a basename to select files to purge.  Ns_RollFileByDate
 *      is a poorly named wrapper for historical reasons (rolling
 *      implies rotating filenames).
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      May remove (many) files.
 *
 *----------------------------------------------------------------------
 */

int
Ns_RollFileByDate(CONST char *file, int max)
{
    return Ns_PurgeFiles(file, max);
}

int
Ns_PurgeFiles(CONST char *file, int max)
{
    File *fiPtr, *files = NULL;
    int   nfiles, status = NS_ERROR;

    /*
     * Get all files matching "file*" pattern.
     */

    nfiles = MatchFiles(file, &files);
    if (nfiles == -1) {
        Ns_Log(Error, "rollfile: failed to match files '%s': %s",
               file, strerror(Tcl_GetErrno()));
        return NS_ERROR;
    }

    /*
     * Purge (any) excessive files after sorting them
     * on descening file mtime.
     */

    if (nfiles >= max) {
        int ii;

        qsort(files, (size_t)nfiles, sizeof(File), CmpFile);
        for (ii = max, fiPtr = files + ii; ii < nfiles; ii++, fiPtr++) {
            if (Unlink(Tcl_GetString(fiPtr->path)) != 0) {
                goto err;
            }
        }
    }

    status = NS_OK;

 err:
    if (nfiles > 0) {
        int ii;

        for (ii = 0, fiPtr = files + ii; ii < nfiles; ii++, fiPtr++) {
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
MatchFiles(CONST char *filename, File **files)
{
    Tcl_Obj          *path, *pathElems, *parent, *patternObj;
    Tcl_Obj          *matched, **matchElems;
    Tcl_GlobTypeData  types;
    Tcl_StatBuf       st;
    File             *fiPtr;
    int               numElems, code;
    char             *pattern;

    /*
     * Obtain fully qualified path of the passed filename
     */

    path = Tcl_NewStringObj(filename, -1);
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

    Tcl_ListObjIndex(NULL, pathElems, numElems - 1, &patternObj);
    Tcl_AppendToObj(patternObj, "*", 1);
    pattern = Tcl_GetString(patternObj);

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

        Tcl_ListObjGetElements(NULL, matched, &numElems, &matchElems);

        if (numElems > 0) {
	    int ii;

            *files = ns_malloc(sizeof(File) * numElems);
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
 *      Stadard qsort() result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CmpFile(const void *arg1, const void *arg2)
{
    File *f1Ptr = (File *) arg1;
    File *f2Ptr = (File *) arg2;

    if (f1Ptr->mtime < f2Ptr->mtime) {
        return 1;
    } else if (f1Ptr->mtime > f2Ptr->mtime) {
        return -1;
    }

    return 0;
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
Unlink(CONST char *file)
{
    int err;
    Tcl_Obj *fileObj;

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
Rename(CONST char *from, CONST char *to)
{
    int err;
    Tcl_Obj *fromObj, *toObj;

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
Exists(CONST char *file)
{
    int exists;

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
