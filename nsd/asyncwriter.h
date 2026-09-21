/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

#ifndef NSD_ASYNCWRITER_H
# define NSD_ASYNCWRITER_H

# include "ns.h"

NS_EXTERN void *
NsAsyncWritePrepare(const char *buffer, size_t nbyte)
    NS_GNUC_NONNULL(1)
    NS_GNUC_MALLOC
    NS_GNUC_WARN_UNUSED_RESULT
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_ReturnCode
NsAsyncWriteSubmit(int fd, void *preparedPtr, void **wakeTokenPtr)
    NS_GNUC_NONNULL(2,3);

NS_EXTERN void
NsAsyncWriteWake(void *wakeTokenPtr)
    NS_GNUC_NONNULL(1);

#endif /* NSD_ASYNCWRITER_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
