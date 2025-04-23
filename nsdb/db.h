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

#ifndef DB_H
#define DB_H

#include "nsdb.h"

NS_EXTERN void            NsDbInitPools(void);
NS_EXTERN void            NsDbInitServer(const char *server);
NS_EXTERN Ns_TclTraceProc NsDbAddCmds, NsDbReleaseHandles;
NS_EXTERN Ns_ReturnCode    NsDbClose(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
NS_EXTERN void             NsDbDisconnect(Ns_DbHandle *handle)
    NS_GNUC_NONNULL(1);
NS_EXTERN struct DbDriver *NsDbGetDriver(const Ns_DbHandle *handle) NS_GNUC_PURE;
NS_EXTERN struct DbDriver *NsDbLoadDriver(const char *driver)
    NS_GNUC_NONNULL(1);
NS_EXTERN void             NsDbLogSql(const Ns_Time *startTime, Ns_DbHandle *handle, const char *sql)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
NS_EXTERN Ns_ReturnCode    NsDbOpen(Ns_DbHandle *handle)
    NS_GNUC_NONNULL(1);
NS_EXTERN void             NsDbDriverInit(const char *server, const struct DbDriver *driverPtr)
    NS_GNUC_NONNULL(2);
NS_EXTERN uintptr_t        NsDbGetSessionId(const Ns_DbHandle *handle) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

#endif


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
