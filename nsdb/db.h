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
