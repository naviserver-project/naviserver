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
    } else if (check != 0) {

        if (( write != 0 && (mode & TCL_WRITABLE) == 0) 
            ||
            (write == 0 && (mode & TCL_READABLE) == 0)) {
            Tcl_AppendResult(interp, "channel \"", chanId, "\" not open for ",
                             write != 0 ? "writing" : "reading", NULL);
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
 *      This routine is used by the AOLserver * routines
 *      to provide access to the underlying socket.
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
        Tcl_AppendResult(interp, "could not get handle for channel: ",
                         chanId, NULL);
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
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cmd != NULL);
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "file backupMax");
        result = TCL_ERROR;

    } else if (Tcl_GetIntFromObj(interp, objv[2], &max) != TCL_OK) {
        result = TCL_ERROR;

    } else if (max <= 0 || max > 1000) {
        Tcl_AppendResult(interp, "invalid max \"", Tcl_GetString(objv[2]),
                         "\": should be > 0 and <= 1000.", NULL);
        result = TCL_ERROR;

    } else {
        if (*cmd == 'p') {
            status = Ns_PurgeFiles(Tcl_GetString(objv[1]), max);
        } else {
            status = Ns_RollFile(Tcl_GetString(objv[1]), max);
        }
        if (status != NS_OK) {
            Tcl_AppendResult(interp, "could not ", cmd, " \"",
                             Tcl_GetString(objv[1]), "\": ",
                             Tcl_PosixError(interp), NULL);
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
 * NsTclMkTempCmd --
 *
 *      Implements ns_mktemp. 
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      Allocates memory for the filename as a TCL_VOLATILE object.
 *
 *----------------------------------------------------------------------
 */

int
NsTclMkTempCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int argc, CONST84 char *argv[])
{
    int result = TCL_OK;
    
    if (argc == 1) {
        char buffer[PATH_MAX] = "";

        snprintf(buffer, sizeof(buffer), "%s/ns-XXXXXX", nsconf.tmpDir);
	Tcl_SetObjResult(interp, Tcl_NewStringObj(mktemp(buffer), -1));

    } else if (argc == 2) {
	char *buffer = ns_strdup(argv[1]);

	Tcl_SetResult(interp, mktemp(buffer), (Tcl_FreeProc *)ns_free);

    } else {
        Tcl_AppendResult(interp, "wrong # of args: should be \"",
                         argv[0], " ?template?\"", NULL);
        result = TCL_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclTmpNamObjCmd --
 *
 *  Implements ns_tmpnam as obj command. 
 *
 *  The fallback definition of L_tmpnam was removed in Tcl on 2015-07-15
 *
 * Results:
 *  Tcl result. 
 *
 * Side effects:
 *  See docs. 
 *
 *----------------------------------------------------------------------
 */
#ifndef L_tmpnam
#   define L_tmpnam	100
#endif

int
NsTclTmpNamObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *CONST* objv)
{
    char buf[L_tmpnam];
    int  result = TCL_OK;
    
    Ns_LogDeprecated(objv, 1, "ns_mktemp ?template?", NULL);

    if (tmpnam(buf) == NULL) {
        Tcl_SetResult(interp, "could not get temporary filename", TCL_STATIC);
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, -1));
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
    int pid, sig, nocomplain = (int)NS_FALSE, result = TCL_OK;

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
    const char *file1, *file2;
    int nocomplain = (int)NS_FALSE, result = TCL_OK;

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
 *      Implements ns_writefp as obj command. 
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

    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "fileid ?nbytes?");
        result = TCL_ERROR;

    } else if (Ns_TclGetOpenChannel(interp, Tcl_GetString(objv[1]), 0, NS_TRUE, &chan) != TCL_OK) {
        result = TCL_ERROR;

    } else if (objc == 3 && Tcl_GetIntFromObj(interp, objv[2], &nbytes) != TCL_OK) {
        result = TCL_ERROR;

    } else if (unlikely(itPtr->conn == NULL)) {
        Tcl_SetResult(interp, "no connection", TCL_STATIC);
        result = TCL_ERROR;
    } else  {
        Ns_ReturnCode status = Ns_ConnSendChannel(itPtr->conn, chan, (size_t)nbytes);
        if (unlikely(status != NS_OK)) {
            Tcl_SetResult(interp, "i/o failed", TCL_STATIC);
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
 *  Implements ns_truncate as obj command. 
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
NsTclTruncateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char *fileString;
    off_t       length = 0;
    int         result = TCL_OK;

    Ns_ObjvSpec args[] = {
	{"file",      Ns_ObjvString, &fileString, NULL},
	{"?length",   Ns_ObjvInt,    &length,  NULL},
	{NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
	result = TCL_ERROR;

    } else if (truncate(fileString, length) != 0) {
        Tcl_AppendResult(interp, "truncate (\"", fileString, "\", ",
                         length == 0 ? "0" : Tcl_GetString(objv[2]),
                         ") failed: ", Tcl_PosixError(interp), NULL);
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
    const char *fileIdString;
    
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
        Tcl_AppendResult(interp, "ftruncate (\"", fileIdString, "\", ",
                         length == 0 ? "0" : Tcl_GetString(objv[2]),
                         ") failed: ", Tcl_PosixError(interp), NULL);
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
        Ns_DStringFree(&ds);
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
    NsInterp       *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    NsRegChan      *regChan = NULL;

    const char     *name = NULL, *chanName = NULL;
    int             isNew, opt;
    bool            shared;
    Tcl_Channel     chan = NULL;

    Tcl_HashTable  *tabPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    static const char *const opts[] = {
        "cleanup", "list", "create", "put", "get", NULL
    };

    enum {
        CCleanupIdx, CListIdx, CCreateIdx, CPutIdx, CGetIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, 
                            "option", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }
    
    switch (opt) {
    case CCreateIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 1, objv, "create channel name");
            return TCL_ERROR;
        }
        chanName = Tcl_GetString(objv[2]);
        chan = Tcl_GetChannel(interp, chanName, NULL);
        if (chan == (Tcl_Channel)NULL) {
            return TCL_ERROR;
        }
        if (Tcl_IsChannelShared(chan) == 1) {
            Tcl_SetResult(interp, "channel is shared", TCL_STATIC);
            return TCL_ERROR;
        }
        name = Tcl_GetString(objv[3]);
        Ns_MutexLock(&servPtr->chans.lock);
        hPtr = Tcl_CreateHashEntry(&servPtr->chans.table, name, &isNew);
        if (isNew != 0) {
            regChan = ns_malloc(sizeof(NsRegChan));
            regChan->name = ns_strdup(chanName);
            regChan->chan = chan;
            Tcl_SetHashValue(hPtr, regChan);
        }
        Ns_MutexUnlock(&servPtr->chans.lock);
        if (isNew == 0) {
            Tcl_AppendResult(interp, "channel \"", Tcl_GetString(objv[3]), 
                             "\" already exists", NULL);
            return TCL_ERROR;
        }
        UnspliceChannel(interp, chan);
        break;
        
    case CGetIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 1, objv, "get name");
            return TCL_ERROR;
        }
        name = Tcl_GetString(objv[2]);
        Ns_MutexLock(&servPtr->chans.lock);
        hPtr = Tcl_FindHashEntry(&servPtr->chans.table, name);
        if (hPtr != NULL) {
            regChan = (NsRegChan*)Tcl_GetHashValue(hPtr);
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&servPtr->chans.lock);
        if (hPtr == NULL) {
            Tcl_AppendResult(interp, "channel \"", name, "\" not found", NULL);
            return TCL_ERROR;
        }
	assert(regChan != NULL);
        SpliceChannel(interp, regChan->chan);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(regChan->name, -1));
        hPtr = Tcl_CreateHashEntry(&itPtr->chans, name, &isNew);
        Tcl_SetHashValue(hPtr, regChan);
        break;
        
    case CPutIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 1, objv, "put name");
            return TCL_ERROR;
        }
        name = Tcl_GetString(objv[2]);
        hPtr = Tcl_FindHashEntry(&itPtr->chans, name);
        if (hPtr == NULL) {
            Tcl_AppendResult(interp, "channel \"", name, "\" not found", NULL);
            return TCL_ERROR;
        }
        regChan = (NsRegChan*)Tcl_GetHashValue(hPtr);
        chan = Tcl_GetChannel(interp, regChan->name, NULL);
        if (chan == (Tcl_Channel)NULL || chan != regChan->chan) {
            Tcl_DeleteHashEntry(hPtr);
            if (chan != regChan->chan) {
                Tcl_SetResult(interp, "channel mismatch", TCL_STATIC);
            }
            return TCL_ERROR;
        }
        UnspliceChannel(interp, regChan->chan);
        Tcl_DeleteHashEntry(hPtr);
        Ns_MutexLock(&servPtr->chans.lock);
        hPtr = Tcl_CreateHashEntry(&servPtr->chans.table, name, &isNew);
        Tcl_SetHashValue(hPtr, regChan);
        Ns_MutexUnlock(&servPtr->chans.lock);
        break;
        
    case CListIdx:
        if (objc != 2 && objc != 3) {
            Tcl_WrongNumArgs(interp, 1, objv, "list ?-shared?");
            return TCL_ERROR;
        }
        shared = (objc == 3);
        if (shared) {
            Ns_MutexLock(&servPtr->chans.lock);
            tabPtr = &servPtr->chans.table; 
        } else {
            tabPtr = &itPtr->chans;
        }
        hPtr = Tcl_FirstHashEntry(tabPtr, &search);
        while (hPtr != NULL) {
            Tcl_AppendElement(interp, Tcl_GetHashKey(tabPtr, hPtr));
            hPtr = Tcl_NextHashEntry(&search);
        }
        if (shared) {
            Ns_MutexUnlock(&servPtr->chans.lock);
        }
        break;
        
    case CCleanupIdx:
        if (objc != 2 && objc != 3) {
            Tcl_WrongNumArgs(interp, 1, objv, "cleanup ?-shared?");
            return TCL_ERROR;
        }
        shared = (objc == 3);
        if (shared) {
            Ns_MutexLock(&servPtr->chans.lock);
            tabPtr = &servPtr->chans.table;
        } else {
            tabPtr = &itPtr->chans;
        }
        hPtr = Tcl_FirstHashEntry(tabPtr, &search);
        while (hPtr != NULL) {
	    regChan = (NsRegChan*)Tcl_GetHashValue(hPtr);
	    assert(regChan != NULL);
            if (shared) {
                Tcl_SpliceChannel(regChan->chan);
                (void) Tcl_UnregisterChannel((Tcl_Interp*)NULL, regChan->chan);
            } else {
                (void) Tcl_UnregisterChannel(interp, regChan->chan);
            }
            ns_free((char *)regChan->name);
            ns_free(regChan);
            Tcl_DeleteHashEntry(hPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        if (shared) {
            Ns_MutexUnlock(&servPtr->chans.lock);
        }
        break;
    default:
        /* unexpected value */
        assert(opt && 0);
        break;
    }

    return TCL_OK;
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

    Tcl_RegisterChannel((Tcl_Interp *) NULL, chan);
    (void) Tcl_UnregisterChannel(interp, chan);

    Tcl_CutChannel(chan);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
