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
 * pidfile.c --
 *
 *          Implement the PID file routines.
 */

#include "nsd.h"

/*
 * Local functions defined in this file.
 */

static Tcl_Obj *GetFile(void);


/*
 *----------------------------------------------------------------------
 *
 * NsCreatePidFile --
 *
 *      Create file with current pid.
 *
 * Results:
 *          None.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

void
NsCreatePidFile(void)
{
    Tcl_Obj     *path;
    Tcl_Channel  chan;

    path = GetFile();
    chan = Tcl_OpenFileChannel(NULL, Tcl_GetString(path), "w", 0644);
    if (chan == NULL) {
        Ns_Log(Error, "pidfile: failed to open pid file '%s': '%s'",
               Tcl_GetString(path), strerror(Tcl_GetErrno()));
    } else if (Tcl_SetChannelOption(NULL, chan, "-translation", "binary")
               != TCL_OK) {
        Ns_Log(Error, "pidfile: failed to set channel option '%s': '%s'",
               Tcl_GetString(path), strerror(Tcl_GetErrno()));
    } else {
        size_t toWrite;
        char   buf[TCL_INTEGER_SPACE + 1];

        snprintf(buf, sizeof(buf), "%d\n", nsconf.pid);
        toWrite = strlen(buf);
        if ((size_t)Tcl_WriteChars(chan, buf, (int)toWrite) != toWrite) {
            Ns_Log(Error, "pidfile: failed to write pid file '%s': '%s'",
                   Tcl_GetString(path), strerror(Tcl_GetErrno()));
        }
        (void) Tcl_Close(NULL, chan);
    }
    Tcl_DecrRefCount(path);
}

/*
 *----------------------------------------------------------------------
 *
 * NsRemovePidFile --
 *
 *      Remove file with current pid.
 *
 * Results:
 *          None.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

void
NsRemovePidFile(void)
{
    Tcl_Obj *path;

    path = GetFile();
    Tcl_IncrRefCount(path);
    if (Tcl_FSDeleteFile(path) != 0) {
        Ns_Log(Error, "pidfile: failed to remove '%s': '%s'",
               Tcl_GetString(path), strerror(Tcl_GetErrno()));
    }
    Tcl_DecrRefCount(path);
}

static Tcl_Obj *
GetFile(void)
{
    const char *file;
    Tcl_Obj *path;

    file = Ns_ConfigGetValue(NS_GLOBAL_CONFIG_PARAMETERS, "pidfile");
    if (file == NULL) {
        Ns_DString ds;
        Ns_Set *set;

        Ns_DStringInit(&ds);
        if (Ns_HomePathExists("logs", (char *)0L)) {
            (void) Ns_HomePath(&ds, "logs/nsd.pid", (char *)0L);
        } else {
            (void) Ns_HomePath(&ds, "nsd.pid", (char *)0L);
        }
        path = Tcl_NewStringObj(ds.string, ds.length);

        set = Ns_ConfigCreateSection(NS_GLOBAL_CONFIG_PARAMETERS);
        Ns_SetUpdateSz(set, "pidfile", 7, ds.string, ds.length);

        Ns_DStringFree(&ds);
    } else {
        path = Tcl_NewStringObj(file, TCL_INDEX_NONE);
    }
    return path;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
