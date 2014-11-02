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
 * nsdb.c --
 *
 *	Database module entry point.
 */

#include "db.h"

NS_EXPORT const int Ns_ModuleVersion = 1;


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *	Module initialization point.
 *
 * Results:
 *	NS_OK or NS_ERROR.
 *
 * Side effects:
 *	May load database drivers and configure pools.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT int
Ns_ModuleInit(char *server, char *module)
{
    static int once = 0;

    if (once == 0) {
	NsDbInitPools();
	once = 1;
    }
    NsDbInitServer(server);
    if (Ns_TclRegisterTrace(server, NsDbAddCmds, server,
                            NS_TCL_TRACE_CREATE) != NS_OK
        || Ns_TclRegisterTrace(server, NsDbReleaseHandles, NULL,
                               NS_TCL_TRACE_DEALLOCATE) != NS_OK) {
        return NS_ERROR;
    }
    Ns_RegisterProcInfo((Ns_Callback *)NsDbAddCmds, "nsdb:initinterp", NULL);
    Ns_RegisterProcInfo((Ns_Callback *)NsDbReleaseHandles, "nsdb:releasehandles", NULL);

    return NS_OK;
}
