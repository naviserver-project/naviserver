#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://www.mozilla.org/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is AOLserver Code and related documentation
# distributed by AOL.
# 
# The Initial Developer of the Original Code is America Online,
# Inc. Portions created by AOL are Copyright (C) 1999 America Online,
# Inc. All Rights Reserved.
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
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
