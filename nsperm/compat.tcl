#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
#

#
# compat.tcl -
#    Compatibility functions
#

proc ns_passwordcheck { user password } {
    ns_deprecated "ns_perm checkpass"
    set ret [catch {ns_perm checkpass $user $password} err]
    if {$ret == 0} {
        return 0
    } else {
        return 1
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
