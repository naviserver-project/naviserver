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
 * tclfile.c --
 *
 *      Tcl commands that do stuff to the filesystem.
 */

#include "nsd.h"

/*
 * Static variables defined in this file.
 */
static Ns_ObjvValueRange posintRange0 = {0, INT_MAX};
static Ns_ObjvValueRange posSizeRange0 = {0, TCL_SIZE_MAX};

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

static int  FileObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *cmd)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

static TCL_OBJCMDPROC_T ChanCleanupObjCmd;
static TCL_OBJCMDPROC_T ChanListObjCmd;
static TCL_OBJCMDPROC_T ChanCreateObjCmd;
static TCL_OBJCMDPROC_T ChanPutObjCmd;
static TCL_OBJCMDPROC_T ChanGetObjCmd;


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
 *      This routine is used by the NaviServer routines to provide
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
 *      Implements "ns_rollfile" and "ns_purgefiles".
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
FileObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, const char *cmd)
{
    int               maxFiles = 0, result;
    Tcl_Obj          *fileObj = NULL;
    Ns_ObjvValueRange range = {0, 1000};

    Ns_ObjvSpec args[] = {
        {"path",        Ns_ObjvObj, &fileObj,  NULL},
        {"maxbackups",  Ns_ObjvInt, &maxFiles, &range},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cmd != NULL);

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        /*
         * All parameters are ok.
         */
        Ns_ReturnCode status;
        const char   *path = Tcl_GetString(fileObj);

        if (*cmd == 'p' /* "purge" */ ) {
            status = Ns_PurgeFiles(path, (TCL_SIZE_T)maxFiles);
        } else /* must be "roll" */ {
            status = Ns_RollFile(path, (TCL_SIZE_T)maxFiles);
        }
        if (status != NS_OK) {
            Ns_TclPrintfResult(interp, "could not %s \"%s\": %s",
                               cmd, path, Tcl_PosixError(interp));
            result = TCL_ERROR;
        } else {
            result = TCL_OK;
        }
    }

    return result;
}

int
NsTclRollFileObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return FileObjCmd(interp, objc, objv, "roll");
}

int
NsTclPurgeFilesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return FileObjCmd(interp, objc, objv, "purge");
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclMkTempObjCmd --
 *
 *      Implements "ns_mktemp". The function generates a unique
 *      temporary filename using optionally a template as argument.
 *
 *      In general, the function mktemp() is not recommended, since
 *      there is a time gap between the generation of a filename and
 *      the generation of a file or directory with the name. This can
 *      result in race conditions or attacks. However, using this
 *      function is still better than home-brewed solutions for the
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
NsTclMkTempObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK, nocomplain = (int)NS_FALSE;
    const char  *templateString = NS_EMPTY_STRING;
    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool,  &nocomplain, INT2PTR(NS_TRUE)},
        {"--",          Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"?template", Ns_ObjvString, &templateString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        char buffer[PATH_MAX] = "";
        int  fd;

        if (*templateString == '\0') {
            snprintf(buffer, sizeof(buffer), "%s/ns-XXXXXX", nsconf.tmpDir);
        } else {
            strncpy(buffer, templateString, PATH_MAX-1);
        }
        fd = ns_mkstemp(buffer);
        if (fd > -1) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(buffer, TCL_INDEX_NONE));
            /*
             * Delete and close the file, that we do not need.
             */
            (void) unlink(buffer);
            (void) close(fd);
            if (nocomplain == (int)NS_FALSE) {
                Tcl_DString ds;

                Tcl_DStringInit(&ds);
                if (*templateString != '\0') {
                    Tcl_DStringAppend(&ds, " ", 1);
                    Tcl_DStringAppend(&ds, templateString, TCL_INDEX_NONE);
                }
                Ns_Log(Deprecated, "'ns_mktemp%s' is deprecated since it poses a potential race condition and security risk;"
                       " consider using 'ns_uuid' or 'file tempfile' instead", ds.string);
                Tcl_DStringFree(&ds);
            }

        } else {
            Ns_TclPrintfResult(interp, "could create file '%s': %s", buffer, ns_sockstrerror(errno));
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclMkdTempObjCmd --
 *
 *      Implements "ns_mkdtemp". The function generates a unique
 *      temporary directory using optionally a template as argument.
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
NsTclMkdTempObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    const char  *templateString = NS_EMPTY_STRING;
    Ns_ObjvSpec  args[] = {
        {"?template", Ns_ObjvString, &templateString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (objc == 1) {
        char buffer[PATH_MAX] = "";

        snprintf(buffer, sizeof(buffer), "%s/nsd-XXXXXX", nsconf.tmpDir);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(ns_mkdtemp(buffer), TCL_INDEX_NONE));

    } else /*if (objc == 2)*/ {
        char *buffer;

        assert(templateString != NULL);
        buffer = ns_strdup(templateString);
        Tcl_SetResult(interp, ns_mkdtemp(buffer), (Tcl_FreeProc *)ns_free);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclKillObjCmd --
 *
 *      Implements "ns_kill".
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
NsTclKillObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         pid = 0, sig = 0, nocomplain = (int)NS_FALSE, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool,  &nocomplain, INT2PTR(NS_TRUE)},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"pid",    Ns_ObjvInt, &pid, NULL},
        {"signal", Ns_ObjvInt, &sig, &posintRange0},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        int rc = kill((pid_t)pid, sig);

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
 *      Implements "ns_symlink".
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
NsTclSymlinkObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *file1, *file2;
    int         nocomplain = (int)NS_FALSE, result;
    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool,  &nocomplain, INT2PTR(NS_TRUE)},
        {"--",          Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"filename1",  Ns_ObjvString, &file1,  NULL},
        {"filename2",  Ns_ObjvString, &file2,  NULL},
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
 *     Implements "ns_writefp".
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
NsTclWriteFpObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Channel chan = NULL;
    Tcl_WideInt nbytes = -1;
    int         result = TCL_OK;

    if (unlikely(objc < 2 || objc > 3)) {
        Tcl_WrongNumArgs(interp, 1, objv, "/channelId/ ?/nbytes/?");
        result = TCL_ERROR;

    } else if (/*objc >= 2*/
               Ns_TclGetOpenChannel(interp, Tcl_GetString(objv[1]),
                                    0, NS_TRUE, &chan) != TCL_OK) {
        result = TCL_ERROR;
    } else if (objc == 3
               && Tcl_GetWideIntFromObj(interp, objv[2], &nbytes) != TCL_OK) {

        result = TCL_ERROR;
    } else if (NsConnRequire(interp, NS_CONN_REQUIRE_ALL, NULL, &result) != NS_OK) {
        /*
         * Might be a soft error.
         */
    } else {

        /*
         * All parameters are ok.
         */

        const NsInterp *itPtr = clientData;
        Ns_ReturnCode   status;

        status = Ns_ConnSendChannel(itPtr->conn, chan, (ssize_t)nbytes);

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
 *     Implements "ns_truncate".
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
NsTclTruncateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char             *fileString;
    int               result = TCL_OK;
    Tcl_WideInt       length = 0;
    Ns_ObjvSpec       args[] = {
        {"filename",  Ns_ObjvString,  &fileString, NULL},
        {"?length",   Ns_ObjvWideInt, &length, &posSizeRange0},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (truncate(fileString, (off_t)length) != 0) {
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
 *      Implements "ns_ftruncate".
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
NsTclFTruncateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int               fd, result = TCL_OK;
    Tcl_WideInt       length = 0;
    char             *fileIdString;
    Ns_ObjvSpec args[] = {
        {"channelId", Ns_ObjvString,  &fileIdString, NULL},
        {"?length",   Ns_ObjvWideInt, &length,       &posSizeRange0},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpenFd(interp, fileIdString, 1, &fd) != TCL_OK) {
        result = TCL_ERROR;

    } else if (ftruncate(fd, (off_t)length) != 0) {
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
 * NsTclFSeekCharsObjCmd --
 *
 *      Search in the open file from the current position for the
 *      provided string. When the string is found, set the result to
 *      the position of the first character. Otherwise set to result
 *      to -1.
 *
 *      Implements "ns_fseekchars".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

#define FSEEKCHARS_BUFFER_SIZE 32768

int
NsTclFSeekCharsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    static const unsigned int bufferSize = FSEEKCHARS_BUFFER_SIZE;
    int               result = TCL_OK;
    Tcl_Channel       channel;
    char             *channelString, *charString;
    Ns_ObjvSpec args[] = {
        {"channelId",    Ns_ObjvString,  &channelString, NULL},
        {"searchstring", Ns_ObjvString,  &charString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (strlen(charString) > bufferSize-1 || *charString == '\0') {
        Ns_TclPrintfResult(interp, "searchstring <%s> must be at least one and at most %d characters", charString, bufferSize-1);
        result = TCL_ERROR;

    } else if (Ns_TclGetOpenChannel(interp, channelString, 0, NS_FALSE, &channel) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_WideInt startPos = Tcl_Tell(channel);
        ClientData  channelData;

        if (Tcl_GetChannelHandle(channel, TCL_READABLE, &channelData) != TCL_OK) {
            Ns_TclPrintfResult(interp, "could not get handle for channel: %s", channelString);
            result = TCL_ERROR;

        } else {
            int     fd = PTR2INT(channelData);
            char    buffer[FSEEKCHARS_BUFFER_SIZE];
            ssize_t bytesRead;
            off_t   offset = 0;
            bool    done = NS_FALSE;
            size_t  searchLength = strlen(charString), moveLength = searchLength - 1, movedSize = 0u;

            /*
             * Initial read
             */
            bytesRead = ns_read(fd, buffer, bufferSize);

            while (bytesRead > 0) {
                char *p;

                /*
                 * Search within the current buffer
                 */
                p = ns_memmem(buffer, (size_t)bytesRead + movedSize, charString, searchLength);
                if (p != NULL) {
                    offset += (p - buffer);
                    done = NS_TRUE;
                    break;
                }
                offset += bytesRead;

                /*
                 * Move the potential overlap part of the buffer to
                 * the beginning.  When bytesRead was already shorter
                 * than the searchLength, then we are at the end of
                 * the file already. The move is not needed.
                 */
                if ((size_t)bytesRead >= searchLength) {
                    /*
                     * The move length is the length of the search
                     * string minus 1. Without the -1, we would have
                     * found the search string already.
                     */
                    memmove(buffer, &buffer[(size_t)bytesRead - moveLength], moveLength);
                    movedSize = moveLength;
                    bytesRead = ns_read(fd, buffer + moveLength, bufferSize - moveLength);
                } else {
                    movedSize = 0;
                    bytesRead = ns_read(fd, buffer, bufferSize);
                }
            }
            if (done) {
                Tcl_WideInt foundPos = startPos + offset;

                Tcl_SetObjResult(interp, Tcl_NewWideIntObj(foundPos));
                Tcl_Seek(channel, foundPos, SEEK_SET) ;
                //Ns_Log(Notice, "......... returning file pos %ld", (long)foundPos);
            } else {
                Tcl_SetObjResult(interp, Tcl_NewIntObj(-1));
            }
        }
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclNormalizePathObjCmd --
 *
 *          Implements "ns_normalizepath".
 *
 * Results:
 *          Tcl result.
 *
 * Side effects:
 *          See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclNormalizePathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_DString ds;
    int        result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/path/");
        result = TCL_ERROR;

    } else {
        Tcl_DStringInit(&ds);
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
 *    Implements "ns_chan create".
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
ChanCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
 *    Implements "ns_chan get".
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
ChanGetObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
            Tcl_SetObjResult(interp, Tcl_NewStringObj(regChan->name, TCL_INDEX_NONE));
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
 *    Implements "ns_chan put".
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
ChanPutObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
 *    Implements "ns_chan list".
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
ChanListObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
            const char *key = Ns_TclGetHashKeyString(tabPtr, hPtr);
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(key, TCL_INDEX_NONE));
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
 *    Implements "ns_chan cleanup".
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
ChanCleanupObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
         * Cleanup every entry found in the hash table.
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
            ns_free_const(regChan->name);
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
 *  Implements "ns_chan".
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
NsTclChanObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
 *      Channel is not accessible by Tcl scripts any more.
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
