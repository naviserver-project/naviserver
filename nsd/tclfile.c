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
 * tclfile.c --
 *
 *      Tcl commands that do stuff to the filesystem.
 */

#include "nsd.h"

/*
 * Structure handling one registered channel for the [ns_chan] command
 */

typedef struct {
    const char  *name;
    Tcl_Channel  chan;
} NsRegChan;

static void SpliceChannel(Tcl_Interp *interp, Tcl_Channel chan)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void UnspliceChannel(Tcl_Interp *interp, Tcl_Channel chan)
        NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int  FileObjCmd(Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, const char *cmd)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

static Tcl_ObjCmdProc ChanCleanupObjCmd;
static Tcl_ObjCmdProc ChanListObjCmd;
static Tcl_ObjCmdProc ChanCreateObjCmd;
static Tcl_ObjCmdProc ChanPutObjCmd;
static Tcl_ObjCmdProc ChanGetObjCmd;


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetOpenChannel --
 *
 *      Return an open channel with an interface similar to the
 *      pre-Tcl7.5 Tcl_GetOpenFile, used throughout the server.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      The value at chanPtr is updated with a valid open Tcl_Channel.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclGetOpenChannel(Tcl_Interp *interp, const char *chanId, int write,
                     bool check, Tcl_Channel *chanPtr)
{
    int mode, result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(chanId != NULL);
    NS_NONNULL_ASSERT(chanPtr != NULL);

    *chanPtr = Tcl_GetChannel(interp, chanId, &mode);

    if (*chanPtr == NULL) {
        result = TCL_ERROR;
    } else if (check) {

        if (( write != 0 && (mode & TCL_WRITABLE) == 0)
            ||
            (write == 0 && (mode & TCL_READABLE) == 0)) {
            Ns_TclPrintfResult(interp, "channel \"%s\" not open for %s",
                               chanId, write != 0 ? "writing" : "reading");
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetOpenFd --
 *
 *      Return an open Unix file descriptor for the given channel.
 *      This routine is used by the AOLserver * routines to provide
 *      access to the underlying socket.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      The value at fdPtr is updated with a valid Unix file descriptor.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclGetOpenFd(Tcl_Interp *interp, const char *chanId, int write, int *fdPtr)
{
    Tcl_Channel chan;
    ClientData  data;
    int         result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(chanId != NULL);
    NS_NONNULL_ASSERT(fdPtr != NULL);

    if (Ns_TclGetOpenChannel(interp, chanId, write, NS_TRUE, &chan) != TCL_OK) {
        result = TCL_ERROR;

    } else if (Tcl_GetChannelHandle(chan, write != 0 ? TCL_WRITABLE : TCL_READABLE,
                             &data) != TCL_OK) {
        Ns_TclPrintfResult(interp, "could not get handle for channel: %s", chanId);
        result = TCL_ERROR;

    } else {
        *fdPtr = PTR2INT(data);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRollFileObjCmd --
 *
 *      Implements ns_rollfile obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

static int
FileObjCmd(Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, const char *cmd)
{
    int           max, result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cmd != NULL);

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "file backupMax");
        result = TCL_ERROR;

    } else if (Tcl_GetIntFromObj(interp, objv[2], &max) != TCL_OK) {
        result = TCL_ERROR;

    } else if (max <= 0 || max > 1000) {
        Ns_TclPrintfResult(interp, "invalid max %d: should be > 0 and <= 1000.", max);
        result = TCL_ERROR;

    } else {
        /*
         * All parameters are ok.
         */
        Ns_ReturnCode status;

        if (*cmd == 'p' /* "purge" */ ) {
            status = Ns_PurgeFiles(Tcl_GetString(objv[1]), max);
        } else /* must be "roll" */ {
            status = Ns_RollFile(Tcl_GetString(objv[1]), max);
        }
        if (status != NS_OK) {
            Ns_TclPrintfResult(interp, "could not %s \"%s\": %s",
                               cmd, Tcl_GetString(objv[1]), Tcl_PosixError(interp));
            result = TCL_ERROR;
        } else {
            result = TCL_OK;
        }
    }

    return result;
}

int
NsTclRollFileObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return FileObjCmd(interp, objc, objv, "roll");
}

int
NsTclPurgeFilesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return FileObjCmd(interp, objc, objv, "purge");
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclMkTempObjCmd --
 *
 *      Implements ns_mktemp. The function generates a unique
 *      temporary filename using optionally a template as argument.
 *
 *      In general, the function mktemp() is not recommended, since
 *      there is a time gap between the generation of a file name and
 *      the generation of a file or directory with the * name. This
 *      can result in race conditions or * attacks. however, using the
 *      finction is still better than * home-brewed solutions for the
 *      same task.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Allocates potentially memory for the filename.
 *
 *----------------------------------------------------------------------
 */
int
NsTclMkTempObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          result = TCL_OK;
    char        *templateString = NULL;
    Ns_ObjvSpec  args[] = {
        {"?template", Ns_ObjvString, &templateString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (objc == 1) {
        char buffer[PATH_MAX] = "";

        snprintf(buffer, sizeof(buffer), "%s/ns-XXXXXX", nsconf.tmpDir);
	Tcl_SetObjResult(interp, Tcl_NewStringObj(mktemp(buffer), -1));

    } else /*if (objc == 2)*/ {
	char *buffer = ns_strdup(templateString);

	Tcl_SetResult(interp, mktemp(buffer), (Tcl_FreeProc *)ns_free);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclKillObjCmd --
 *
 *      Implements ns_kill as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclKillObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         pid, sig, nocomplain = (int)NS_FALSE, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool,  &nocomplain, INT2PTR(NS_TRUE)},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"pid",  Ns_ObjvInt, &pid,    NULL},
        {"sig",  Ns_ObjvInt, &sig,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        int rc = kill(pid, sig);

        if (rc != 0 && !nocomplain) {
            Ns_TclPrintfResult(interp, "kill %d %d failed: %s", pid, sig, Tcl_PosixError(interp));
            result = TCL_ERROR;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSymlinkObjCmd --
 *
 *      Implements ns_symlink as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclSymlinkObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    char       *file1, *file2;
    int         nocomplain = (int)NS_FALSE, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool,  &nocomplain, INT2PTR(NS_TRUE)},
        {"--",          Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"file1",  Ns_ObjvString, &file1,  NULL},
        {"file2",  Ns_ObjvString, &file2,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        int rc = result = symlink(file1, file2);
        if (rc != 0 && !nocomplain) {
            Ns_TclPrintfResult(interp, "symlink '%s' '%s' failed: %s", file1, file2,
                               Tcl_PosixError(interp));
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclWriteFpObjCmd --
 *
 *     Implements ns_writefp as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclWriteFpObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    Tcl_Channel     chan;
    int             nbytes = INT_MAX, result = TCL_OK;
    char           *fileidString;
    Ns_ObjvSpec     args[] = {
        {"fileid", Ns_ObjvString, &fileidString, NULL},
        {"nbytes", Ns_ObjvInt,    &nbytes, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (NsConnRequire(interp, NULL) != NS_OK
        || Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (Ns_TclGetOpenChannel(interp, fileidString, 0, NS_TRUE, &chan) != TCL_OK) {
        result = TCL_ERROR;

    } else  {
        /*
         * All parameters are ok.
         */
        Ns_ReturnCode status = Ns_ConnSendChannel(itPtr->conn, chan, (size_t)nbytes);

        if (unlikely(status != NS_OK)) {
            Ns_TclPrintfResult(interp, "I/O failed");
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclTruncateObjCmd --
 *
 *     Implements ns_truncate as obj command.
 *
 * Results:
 *     Tcl result.
 *
 * Side effects:
 *     See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclTruncateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    char    *fileString;
    off_t    length = 0;
    int      result = TCL_OK;

    Ns_ObjvSpec args[] = {
	{"file",      Ns_ObjvString, &fileString, NULL},
	{"?length",   Ns_ObjvInt,    &length,  NULL},
	{NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
	result = TCL_ERROR;

    } else if (truncate(fileString, length) != 0) {
        Ns_TclPrintfResult(interp, "truncate (\"%s\", %s) failed: %s",
                           fileString,
                           length == 0 ? "0" : Tcl_GetString(objv[2]),
                           Tcl_PosixError(interp));
        result = TCL_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclFTruncateObjCmd --
 *
 *      Implements ns_ftruncate as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclFTruncateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         fd, result = TCL_OK;
    off_t       length = 0;
    char       *fileIdString;
    Ns_ObjvSpec args[] = {
	{"fileId",    Ns_ObjvString, &fileIdString, NULL},
	{"?length",   Ns_ObjvInt,    &length,  NULL},
	{NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
	result = TCL_ERROR;

    } else if (Ns_TclGetOpenFd(interp, fileIdString, 1, &fd) != TCL_OK) {
        result = TCL_ERROR;

    } else if (ftruncate(fd, length) != 0) {
        Ns_TclPrintfResult(interp, "ftruncate (\"%s\", %s) failed: %s",
                           fileIdString,
                           length == 0 ? "0" : Tcl_GetString(objv[2]),
                           Tcl_PosixError(interp));
        result = TCL_ERROR;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclNormalizePathObjCmd --
 *
 *	    Implements ns_normalizepath as obj command.
 *
 * Results:
 *	    Tcl result.
 *
 * Side effects:
 *	    See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclNormalizePathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_DString ds;
    int        result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "path");
        result = TCL_ERROR;

    } else {
        Ns_DStringInit(&ds);
        Ns_NormalizePath(&ds, Tcl_GetString(objv[1]));
        Tcl_DStringResult(interp, &ds);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ChanCreateObjCmd --
 *
 *    Implement the "ns_chan create" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    See docs.
 *
 *----------------------------------------------------------------------
 */
static int
ChanCreateObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    char           *name, *chanName;
    int             result = TCL_OK;
    Ns_ObjvSpec     args[] = {
        {"channel", Ns_ObjvString, &chanName, NULL},
        {"name",    Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_Channel chan;

        chan = Tcl_GetChannel(interp, chanName, NULL);
        if (chan == (Tcl_Channel)NULL) {
            result = TCL_ERROR;

        } else if (Tcl_IsChannelShared(chan) == 1) {
            Ns_TclPrintfResult(interp, "channel is shared");
            result = TCL_ERROR;

        } else {
            /*
             * All parameters are ok.
             */
            const NsInterp *itPtr = clientData;
            NsServer       *servPtr = itPtr->servPtr;
            Tcl_HashEntry  *hPtr;
            int             isNew;

            Ns_MutexLock(&servPtr->chans.lock);
            hPtr = Tcl_CreateHashEntry(&servPtr->chans.table, name, &isNew);
            if (isNew != 0) {
                NsRegChan *regChan;

                /*
                 * Allocate a new NsRegChan entry.
                 */
                regChan = ns_malloc(sizeof(NsRegChan));
                regChan->name = ns_strdup(chanName);
                regChan->chan = chan;
                Tcl_SetHashValue(hPtr, regChan);
            }
            Ns_MutexUnlock(&servPtr->chans.lock);
            if (isNew == 0) {
                Ns_TclPrintfResult(interp, "channel \"%s\" already exists",
                                   name);
                result = TCL_ERROR;
            } else {
                UnspliceChannel(interp, chan);
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ChanGetObjCmd --
 *
 *    Implement the "ns_chan get" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    See docs.
 *
 *----------------------------------------------------------------------
 */
static int
ChanGetObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    char        *name;
    int          result = TCL_OK;
    Ns_ObjvSpec  args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_HashEntry *hPtr;
        NsInterp      *itPtr = clientData;
        NsServer      *servPtr = itPtr->servPtr;
        NsRegChan     *regChan = NULL;

        Ns_MutexLock(&servPtr->chans.lock);
        hPtr = Tcl_FindHashEntry(&servPtr->chans.table, name);
        if (hPtr != NULL) {
            regChan = (NsRegChan*)Tcl_GetHashValue(hPtr);
            Tcl_DeleteHashEntry(hPtr);
            assert(regChan != NULL);
        }
        Ns_MutexUnlock(&servPtr->chans.lock);

        if (hPtr == NULL) {
            Ns_TclPrintfResult(interp, "channel \"%s\" not found", name);
            result = TCL_ERROR;
        } else {
            int isNew;

            /*
             * We have a valid NsRegChan.
             */
            assert(regChan != NULL);
            SpliceChannel(interp, regChan->chan);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(regChan->name, -1));
            hPtr = Tcl_CreateHashEntry(&itPtr->chans, name, &isNew);
            Tcl_SetHashValue(hPtr, regChan);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ChanPutObjCmd --
 *
 *    Implement the "ns_chan put" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    See docs.
 *
 *----------------------------------------------------------------------
 */
static int
ChanPutObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    char         *name;
    int           result = TCL_OK;
    Ns_ObjvSpec   args[] = {
        {"name",     Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        NsInterp       *itPtr = clientData;
        Tcl_HashEntry  *hPtr = Tcl_FindHashEntry(&itPtr->chans, name);

        if (hPtr == NULL) {
            Ns_TclPrintfResult(interp, "channel \"%s\" not found", name);
            result = TCL_ERROR;

        } else {
            NsRegChan   *regChan;
            Tcl_Channel  chan;

            regChan = (NsRegChan*)Tcl_GetHashValue(hPtr);
            chan = Tcl_GetChannel(interp, regChan->name, NULL);
            if (chan == NULL || chan != regChan->chan) {
                Tcl_DeleteHashEntry(hPtr);
                if (chan != regChan->chan) {
                    Ns_TclPrintfResult(interp, "channel mismatch");
                }
                result = TCL_ERROR;
            } else {
                NsServer *servPtr = itPtr->servPtr;
                int       isNew;

                /*
                 * We have a valid NsRegChan.
                 */
                UnspliceChannel(interp, regChan->chan);
                Tcl_DeleteHashEntry(hPtr);
                Ns_MutexLock(&servPtr->chans.lock);
                hPtr = Tcl_CreateHashEntry(&servPtr->chans.table, name, &isNew);
                Tcl_SetHashValue(hPtr, regChan);
                Ns_MutexUnlock(&servPtr->chans.lock);
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ChanListObjCmd --
 *
 *    Implement the "ns_chan list" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    See docs.
 *
 *----------------------------------------------------------------------
 */
static int
ChanListObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK, isShared = (int)NS_FALSE;
    Ns_ObjvSpec lopts[] = {
        {"-shared",  Ns_ObjvBool, &isShared, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;
        NsInterp            *itPtr = clientData;
        NsServer            *servPtr = itPtr->servPtr;
        Tcl_HashTable       *tabPtr;
        Tcl_Obj             *listObj = Tcl_NewListObj(0, NULL);

        if (isShared != (int)NS_FALSE) {
            Ns_MutexLock(&servPtr->chans.lock);
            tabPtr = &servPtr->chans.table;
        } else {
            tabPtr = &itPtr->chans;
        }

        /*
         * Compute a Tcl list of the keys of every entry of the hash
         * table.
         */
        for (hPtr = Tcl_FirstHashEntry(tabPtr, &search); hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search)) {
            const char *key = Tcl_GetHashKey(tabPtr, hPtr);
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(key, -1));
        }
        if (isShared != (int)NS_FALSE) {
            Ns_MutexUnlock(&servPtr->chans.lock);
        }
        Tcl_SetObjResult(interp, listObj);

    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ChanCleanupObjCmd --
 *
 *    Implement the "ns_chan cleanup" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    See docs.
 *
 *----------------------------------------------------------------------
 */
static int
ChanCleanupObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK, isShared = (int)NS_FALSE;
    Ns_ObjvSpec lopts[] = {
        {"-shared",  Ns_ObjvBool, &isShared, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        NsInterp       *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        Tcl_HashTable  *tabPtr;
        Tcl_HashEntry  *hPtr;
        Tcl_HashSearch  search;

        if (isShared != (int)NS_FALSE) {
            Ns_MutexLock(&servPtr->chans.lock);
            tabPtr = &servPtr->chans.table;
        } else {
            tabPtr = &itPtr->chans;
        }

        /*
         * Cleanup every entry found in the the hash table.
         */
        for (hPtr = Tcl_FirstHashEntry(tabPtr, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
            NsRegChan *regChan;

            regChan = (NsRegChan*)Tcl_GetHashValue(hPtr);
            assert(regChan != NULL);
            if (isShared != (int)NS_FALSE) {
                Tcl_SpliceChannel(regChan->chan);
                (void) Tcl_UnregisterChannel((Tcl_Interp*)NULL, regChan->chan);
            } else {
                (void) Tcl_UnregisterChannel(interp, regChan->chan);
            }
            ns_free((char *)regChan->name);
            ns_free(regChan);
            Tcl_DeleteHashEntry(hPtr);
        }
        if (isShared != (int)NS_FALSE) {
            Ns_MutexUnlock(&servPtr->chans.lock);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclChanObjCmd --
 *
 *  Implement the ns_chan command.
 *
 * Results:
 *  Tcl result.
 *
 * Side effects:
 *  See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclChanObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"cleanup", ChanCleanupObjCmd},
        {"list",    ChanListObjCmd},
        {"create",  ChanCreateObjCmd},
        {"put",     ChanPutObjCmd},
        {"get",     ChanGetObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * SpliceChannel
 *
 *      Adds the shared channel in the interp/thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      New channel appears in the interp.
 *
 *----------------------------------------------------------------------
 */

static void
SpliceChannel(Tcl_Interp *interp, Tcl_Channel chan)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(chan != NULL);

    Tcl_SpliceChannel(chan);
    Tcl_RegisterChannel(interp, chan);
    (void) Tcl_UnregisterChannel((Tcl_Interp*)NULL, chan);
}


/*
 *----------------------------------------------------------------------
 *
 * UnspliceChannel
 *
 *      Divorces the channel from its owning interp/thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Channel is not accesible by Tcl scripts any more.
 *
 *----------------------------------------------------------------------
 */

static void
UnspliceChannel(Tcl_Interp *interp, Tcl_Channel chan)
{
    const Tcl_ChannelType *chanTypePtr;
    Tcl_DriverWatchProc   *watchProc;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(chan != NULL);

    Tcl_ClearChannelHandlers(chan);

    chanTypePtr = Tcl_GetChannelType(chan);
    watchProc   = Tcl_ChannelWatchProc(chanTypePtr);

    /*
     * This effectively disables processing of pending
     * events which are ready to fire for the given
     * channel. If we do not do this, events will hit
     * the detached channel which is potentially being
     * owned by some other thread. This will wreck havoc
     * on our memory and eventually badly hurt us...
     */

    if (watchProc != NULL) {
        (*watchProc)(Tcl_GetChannelInstanceData(chan), 0);
    }

    /*
     * Artificially bump the channel reference count
     * which protects us from channel being closed
     * during the Tcl_UnregisterChannel().
     */

    Tcl_RegisterChannel(NULL, chan);
    (void) Tcl_UnregisterChannel(interp, chan);

    Tcl_CutChannel(chan);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
