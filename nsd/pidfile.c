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
 * pidfile.c --
 *
 *	    Implement the PID file routines.
 */

#include "nsd.h"

/*
 * Local functions defined in this file.
 */

static Tcl_Obj *GetFile(char *procname);


/*
 *----------------------------------------------------------------------
 *
 * NsCreatePidFile --
 *
 *      Create file with current pid.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

void
NsCreatePidFile(char *procname)
{
    Tcl_Obj     *path;
    Tcl_Channel  chan;

    path = GetFile(procname);
    chan = Tcl_OpenFileChannel(NULL, Tcl_GetString(path), "w", 0644);
    if (chan == NULL) {
    	Ns_Log(Error, "pidfile: failed to open pid file '%s': '%s'",
               Tcl_GetString(path), strerror(Tcl_GetErrno()));
    } else if (Tcl_SetChannelOption(NULL, chan, "-translation", "binary")
               != TCL_OK) {
    	Ns_Log(Error, "pidfile: failed to set channel option '%s': '%s'",
               Tcl_GetString(path), strerror(Tcl_GetErrno()));
    } else {
        size_t towrite;
        char   buf[TCL_INTEGER_SPACE + 1];

        snprintf(buf, sizeof(buf), "%d\n", nsconf.pid);
        towrite = strlen(buf);
        if ((size_t)Tcl_WriteChars(chan, buf, (int)towrite) != towrite) {
            Ns_Log(Error, "pidfile: failed to write pid file '%s': '%s'",
                   Tcl_GetString(path), strerror(Tcl_GetErrno()));
        }
        Tcl_Close(NULL, chan);
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
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

void
NsRemovePidFile(char *procname)
{
    Tcl_Obj *path;

    path = GetFile(procname);
    Tcl_IncrRefCount(path);
    if (Tcl_FSDeleteFile(path) != 0) {
    	Ns_Log(Error, "pidfile: failed to remove '%s': '%s'",
               Tcl_GetString(path), strerror(Tcl_GetErrno()));
    }
    Tcl_DecrRefCount(path);
}

static Tcl_Obj *
GetFile(char *procname)
{
    char *file;
    Tcl_Obj *path;

    file = Ns_ConfigGetValue(NS_CONFIG_PARAMETERS, "pidfile");
    if (file == NULL) {
        Ns_DString ds;
        Ns_DStringInit(&ds);
        if (Ns_HomePathExists("logs", NULL)) {
            Ns_HomePath(&ds, "logs/nsd.pid", NULL);
        } else {
            Ns_HomePath(&ds, "nsd.pid", NULL);
        }
        path = Tcl_NewStringObj(ds.string, -1);
        Ns_DStringFree(&ds);
    } else {
        path = Tcl_NewStringObj(file, -1);
    }
    return path;
}
