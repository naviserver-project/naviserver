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
 * nsdb.c --
 *
 *      Database module entry point.
 */

#include "db.h"

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Module initialization point.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      May load database drivers and configure pools.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *UNUSED(module))
{
    Ns_ReturnCode status = NS_OK;
    static bool   initialized = NS_FALSE;

    if (!initialized) {
        Ns_LogSqlDebug = Ns_CreateLogSeverity("Debug(sql)");
        NsDbInitPools();
        initialized = NS_TRUE;
    }
    NsDbInitServer(server);
    if (Ns_TclRegisterTrace(server, NsDbAddCmds, server,
                            NS_TCL_TRACE_CREATE) != NS_OK
        || Ns_TclRegisterTrace(server, NsDbReleaseHandles, NULL,
                               NS_TCL_TRACE_DEALLOCATE) != NS_OK) {
        status = NS_ERROR;
    } else {
        Ns_RegisterProcInfo((ns_funcptr_t)NsDbAddCmds, "nsdb:initinterp", NULL);
        Ns_RegisterProcInfo((ns_funcptr_t)NsDbReleaseHandles, "nsdb:releasehandles", NULL);
    }
    return status;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
