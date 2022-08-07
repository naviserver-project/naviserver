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
# limits.tcl --
#
#      Configure request limits (maxupload, timeout, etc.)
#


#
# Create process-global limits.
#

ns_runonce -global {

    set limits [ns_configsection "ns/limits"]

    if {$limits ne ""} {

        foreach {limit description} [ns_set array $limits] {

            set path "ns/limit/$limit"

            if {[catch {
                array set l [ns_limits_set \
                    -maxrun    [ns_config -int -set $path maxrun    100] \
                    -maxwait   [ns_config -int -set $path maxwait   100] \
                    -maxupload [ns_config -int -set $path maxupload 10240000] \
                    -timeout   [ns_config -int -set $path timeout   60] \
                    $limit ]
            } errmsg]} {
                ns_log error limits: $errmsg
            } else {
                ns_log notice limits: $limit: \
                    maxrun=$l(maxrun) maxwait=$l(maxwait) \
                    maxupload=$l(maxupload) timeout=$l(timeout)
            }
        }
    }
}


#
# Map limits for method/url's on this virtual server.
#
# NB: If no limits are created or registered then the default,
#     automatically created, limits apply.
#

set server [ns_info server]
set limits [ns_configsection "ns/server/$server/limits"]

if {$limits ne ""} {
    foreach {limit map} [ns_set array $limits] {
        set method [lindex $map 0]
        set url    [lindex $map 1]
        if {[catch {
            ns_limits_register $limit $method $url
        } errmsg]} {
            ns_log error limits\[$server\]: $errmsg
        } else {
            ns_log notice limits\[$server\]: $limit -> $method $url
        }
    }
}
