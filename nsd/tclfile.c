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

NS_RCSID("@(#) $Header$");


/*
 * Structure handling one registered channel for the [ns_chan] command
 */

typedef struct _NsRegChan {
    char        *name;
    Tcl_Channel  chan;
} NsRegChan;

static void SpliceChannel   (Tcl_Interp *interp, Tcl_Channel chan);
static void UnspliceChannel (Tcl_Interp *interp, Tcl_Channel chan);


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

static int
GetOpenChannel(Tcl_Interp *interp, Tcl_Obj *obj, int write,
               int check, Tcl_Channel *chanPtr)
{
    return Ns_TclGetOpenChannel(interp, Tcl_GetString(obj),
                                write, check, chanPtr);
}

int
Ns_TclGetOpenChannel(Tcl_Interp *interp, char *chanId, int write,
                     int check, Tcl_Channel *chanPtr)
{
    int mode;

    *chanPtr = Tcl_GetChannel(interp, chanId, &mode);

    if (*chanPtr == NULL) {
        return TCL_ERROR;
    }
    if (check == 0) {
        return TCL_OK;
    }
    if (( write && !(mode & TCL_WRITABLE)) 
        ||
        (!write && !(mode & TCL_READABLE))) {
        Tcl_AppendResult(interp, "channel \"", chanId, "\" not open for ",
                         write ? "writing" : "reading", NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
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
Ns_TclGetOpenFd(Tcl_Interp *interp, char *chanId, int write, int *fdPtr)
{
    Tcl_Channel chan;
    ClientData  data;

    if (Ns_TclGetOpenChannel(interp, chanId, write, 1, &chan) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_GetChannelHandle(chan, write ? TCL_WRITABLE : TCL_READABLE,
                             (ClientData*) &data) != TCL_OK) {
        Tcl_AppendResult(interp, "could not get handle for channel: ",
                         chanId, NULL);
        return TCL_ERROR;
    }

    *fdPtr = (int) data;

    return TCL_OK;
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
FileObjCmd(Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[], char *cmd)
{
    int max, status;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "file backupMax");
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &max) != TCL_OK) {
        return TCL_ERROR;
    }
    if (max <= 0 || max > 1000) {
        Tcl_AppendResult(interp, "invalid max \"", Tcl_GetString(objv[2]),
                         "\": should be > 0 and <= 1000.", NULL);
        return TCL_ERROR;
    }
    if (*cmd == 'p') {
        status = Ns_PurgeFiles(Tcl_GetString(objv[1]), max);
    } else {
        status = Ns_RollFile(Tcl_GetString(objv[1]), max);
    }
    if (status != NS_OK) {
        Tcl_AppendResult(interp, "could not ", cmd, " \"",
                         Tcl_GetString(objv[1]), "\": ",
                         Tcl_PosixError(interp), NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
}

int
NsTclRollFileObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                    Tcl_Obj *CONST objv[])
{
    return FileObjCmd(interp, objc, objv, "roll");
}

int
NsTclPurgeFilesObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                      Tcl_Obj *CONST objv[])
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
NsTclMkTempCmd(ClientData dummy, Tcl_Interp *interp, int argc, char **argv)
{
    char *buffer;

    if (argc != 2) {
        Tcl_AppendResult(interp, "wrong # of args: should be \"",
                         argv[0], " template\"", NULL);
        return TCL_ERROR;
    }
    
    buffer = ns_strdup(argv[1]);
    Tcl_SetResult(interp, mktemp(buffer), (Tcl_FreeProc *)ns_free);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclTmpNamObjCmd --
 *
 *  Implements ns_tmpnam as obj command. 
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
NsTclTmpNamObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                  Tcl_Obj *CONST objv[])
{
    char buf[L_tmpnam];

    if (tmpnam(buf) == NULL) {
        Tcl_SetResult(interp, "could not get temporary filename", TCL_STATIC);
        return TCL_ERROR;
    }
    Tcl_SetResult(interp, buf, TCL_VOLATILE);

    return TCL_OK;
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
 *      See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclUrl2FileObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                    Tcl_Obj *CONST objv[])
{
    NsInterp   *itPtr = arg;
    Ns_DString  ds;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "url");
        return TCL_ERROR;
    }
    
    Ns_DStringInit(&ds);
    NsUrlToFile(&ds, itPtr->servPtr, Tcl_GetString(objv[1]));
    Tcl_SetResult(interp, ds.string, TCL_VOLATILE);
    Ns_DStringFree(&ds);

    return TCL_OK;
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
NsTclKillObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                Tcl_Obj *CONST objv[])
{
    int pid, signal;

    if ((objc != 3) && (objc != 4)) {
    badargs:
        Tcl_WrongNumArgs(interp, 1, objv, "?-nocomplain? pid signal");
        return TCL_ERROR;
    }
    if (objc == 3) {
        if (Tcl_GetIntFromObj(interp, objv[1], &pid) != TCL_OK) {
            return TCL_ERROR;
        }
        if (Tcl_GetIntFromObj(interp, objv[2], &signal) != TCL_OK) {
            return TCL_ERROR;
        }
        if (kill(pid, signal) != 0) {
            Tcl_AppendResult(interp, "kill (\"", 
                             Tcl_GetString(objv[1]), ",", 
                             Tcl_GetString(objv[2]), "\") failed: ", 
                             Tcl_PosixError(interp), NULL);
            return TCL_ERROR;
        }
    } else {
        if (strcmp(Tcl_GetString(objv[1]), "-nocomplain") != 0) {
            goto badargs;
        }
        if (Tcl_GetIntFromObj(interp, objv[2], &pid) != TCL_OK) {
            return TCL_ERROR;
        }
        if (Tcl_GetIntFromObj(interp, objv[3], &signal) != TCL_OK) {
            return TCL_ERROR;
        }
        kill(pid, signal);
    }

    return TCL_OK;
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
NsTclSymlinkObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                   Tcl_Obj *CONST objv[])
{
    if ((objc != 3) && (objc != 4)) {
    badargs:
        Tcl_WrongNumArgs(interp, 1, objv, "?-nocomplain? file1 file2");
        return TCL_ERROR;
    }
    
    if (objc == 3) {
        if (symlink(Tcl_GetString(objv[1]), Tcl_GetString(objv[2])) != 0) {
            Tcl_AppendResult(interp, "symlink (\"",
                             Tcl_GetString(objv[1]), "\", \"",
                             Tcl_GetString(objv[2]), "\") failed: ",
                             Tcl_PosixError(interp), NULL);
            return TCL_ERROR;
        }
    } else {
        if (strcmp(Tcl_GetString(objv[1]), "-nocomplain") != 0) {
            goto badargs;
        }
        symlink(Tcl_GetString(objv[2]), Tcl_GetString(objv[3]));
    }
    
    return TCL_OK;
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
NsTclWriteFpObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                   Tcl_Obj *CONST objv[])
{
    NsInterp   *itPtr = arg;
    Tcl_Channel chan;
    int         nbytes = INT_MAX;
    int         result;

    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "fileid ?nbytes?");
        return TCL_ERROR;
    }
    if (GetOpenChannel(interp, objv[1], 0, 1, &chan) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 3 && Tcl_GetIntFromObj(interp, objv[2], &nbytes) != TCL_OK) {
        return TCL_ERROR;
    }
    if (itPtr->conn == NULL) {
        Tcl_SetResult(interp, "no connection", TCL_STATIC);
        return TCL_ERROR;
    }
    result = Ns_ConnSendChannel(itPtr->conn, chan, nbytes);
    if (result != NS_OK) {
        Tcl_SetResult(interp, "i/o failed", TCL_STATIC);
        return TCL_ERROR;
    }

    return TCL_OK;
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
NsTclTruncateObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                    Tcl_Obj *CONST objv[])
{
    int length;
    
    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "file ?length?");
        return TCL_ERROR;
    }
    if (objc == 2) {
        length = 0;
    } else if (Tcl_GetIntFromObj(interp, objv[2], &length) != TCL_OK) {
        return TCL_ERROR;
    }
    if (truncate(Tcl_GetString(objv[1]), length) != 0) {
        Tcl_AppendResult(interp, "truncate (\"", 
                         Tcl_GetString(objv[1]), "\", ",
                         Tcl_GetString(objv[2]) ? 
                         Tcl_GetString(objv[2]) : "0", ") failed: ", 
                         Tcl_PosixError(interp), NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
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
NsTclFTruncateObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                     Tcl_Obj *CONST objv[])
{
    int length, fd;
    
    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "fileId ?length?");
        return TCL_ERROR;
    }
    if (Ns_TclGetOpenFd(interp, Tcl_GetString(objv[1]), 1, &fd) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 2) {
        length = 0;
    } else if (Tcl_GetInt(interp, Tcl_GetString(objv[2]), &length) != TCL_OK) {
        return TCL_ERROR;
    }
    if (ftruncate(fd, length) != 0) {
        Tcl_AppendResult(interp, "ftruncate (\"", 
                         Tcl_GetString(objv[1]), "\", ",
                         Tcl_GetString(objv[2]) ? 
                         Tcl_GetString(objv[2]) : "0", ") failed: ", 
                         Tcl_PosixError(interp), NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
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
NsTclChanObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                Tcl_Obj *CONST objv[])
{
    NsInterp       *itPtr = arg;
    NsServer       *servPtr = itPtr->servPtr;
    NsRegChan      *regChan = NULL;

    char           *name = NULL, *chanName = NULL;
    int             new, shared, opt;
    Tcl_Channel     chan = NULL;

    Tcl_HashTable  *tabPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    static CONST char *opts[] = {
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
        if (Tcl_IsChannelShared(chan)) {
            Tcl_SetResult(interp, "channel is shared", TCL_STATIC);
            return TCL_ERROR;
        }
        name = Tcl_GetString(objv[3]);
        Ns_MutexLock(&servPtr->chans.lock);
        hPtr = Tcl_CreateHashEntry(&servPtr->chans.table, name, &new);
        if (new) {
            regChan = ns_malloc(sizeof(NsRegChan));
            regChan->name = ns_malloc(strlen(chanName)+1);
            regChan->chan = chan;
            strcpy(regChan->name, chanName);
            Tcl_SetHashValue(hPtr, regChan);
        }
        Ns_MutexUnlock(&servPtr->chans.lock);
        if (!new) {
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
        SpliceChannel(interp, regChan->chan);
        Tcl_SetResult(interp, regChan->name, TCL_VOLATILE);
        hPtr = Tcl_CreateHashEntry(&itPtr->chans, name, &new);
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
        hPtr = Tcl_CreateHashEntry(&servPtr->chans.table, name, &new);
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
            if (shared) {
                Tcl_SpliceChannel(regChan->chan);
                Tcl_UnregisterChannel((Tcl_Interp*)NULL, regChan->chan);
            } else {
                Tcl_UnregisterChannel(interp, regChan->chan);
            }
            ns_free(regChan->name);
            ns_free(regChan);
            Tcl_DeleteHashEntry(hPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        if (shared) {
            Ns_MutexUnlock(&servPtr->chans.lock);
        }
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
    Tcl_SpliceChannel(chan);
    Tcl_RegisterChannel(interp, chan);
    Tcl_UnregisterChannel((Tcl_Interp*)NULL, chan);
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
    Tcl_ChannelType *chanTypePtr;
    Tcl_DriverWatchProc *watchProc;

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

    if (watchProc) {
        (*watchProc)(Tcl_GetChannelInstanceData(chan), 0);
    }

    /*
     * Artificially bump the channel reference count
     * which protects us from channel being closed
     * during the Tcl_UnregisterChannel().
     */

    Tcl_RegisterChannel((Tcl_Interp *) NULL, chan);
    Tcl_UnregisterChannel(interp, chan);

    Tcl_CutChannel(chan);
}
