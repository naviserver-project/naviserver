/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef NSLOG_SCAN_H
# define NSLOG_SCAN_H

/* Borrowed configuration snapshot, valid for one ensemble invocation. */
typedef struct NsLogScanDefaults {
    const char *filename;
    Tcl_Obj    *extendedHeaders;
    Tcl_WideInt device, inode;
    bool        haveIdentity, combined, requestTime, partialTimes;
} NsLogScanDefaults;

/* Private scanner entry point; shared batches use the optional nsdb provider API. */
int NsLogScanObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const *objv,
                    const NsLogScanDefaults *defaults)
    NS_GNUC_NONNULL(1, 3, 4);
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
