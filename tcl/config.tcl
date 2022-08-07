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
# config.tcl --
#
#   Configure the various subsystems of the server.
#


#
# Configure the process-global subsystems (once).
#

proc _ns_config_global {} {
    ns_runonce -global {
        _ns_config_global_limits
    }
}

#
# Configure subsystems for this virtual server.
#

proc _ns_config_server {server} {
    _ns_config_server_limits $server
    _ns_config_server_adp_pages $server
    _ns_config_server_tcl_pages $server
}

#
# _ns_config_global_limits --
#
#   Configure global limit definitions.
#

proc _ns_config_global_limits {} {

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
# _ns_config_server_limits --
#
#   Map global limits for method/url combos on a virtual server.
#
#   NB: If no limits are created or registered then the default,
#       automatically created, limits apply.
#

proc _ns_config_server_limits {server} {

    set limits [ns_configsection "ns/server/$server/limits"]

    if {$limits ne ""} {
        foreach {limit map} [ns_set array $limits] {
            set method [lindex $map 0]
            set url    [lindex $map 1]
            if {[catch {
                ns_limits_register $limit $method $url
            } errmsg]} {
                ns_log error "limits\[$server\]: $errmsg"
            } else {
                ns_log notice "limits\[$server\]: $limit -> $method $url"
            }
        }
    }
}

#
# Register ADP page handlers for GET, HEAD and POST
# requests, if enabled.
#

proc _ns_config_server_adp_pages {server} {

    set path "ns/server/$server/adp"
    set adps [ns_configsection $path]

    if {$adps eq "" || [ns_config -bool $path disabled false]} {
        return
    }
    foreach {key url} [ns_set array $adps] {
        if {$key eq "map"} {
            foreach {method} {GET HEAD POST} {
                ns_register_adp $method $url
            }
            ns_log notice "adp\[$server\]: mapped {GET HEAD POST} $url"
        }
    }
}

#
# Register Tcl page handlers for GET, HEAD and POST
# requests, if enabled.
#

proc _ns_config_server_tcl_pages {server} {

    if {[ns_config -bool -set "ns/server/$server/adp" enabletclpages false]} {
        foreach {method} {GET HEAD POST} {
            ns_register_tcl $method /*.tcl
        }
        ns_log notice "tcl\[$server\]: mapped {GET HEAD POST} *.tcl"
    }
}

#
# Configure the server.
#

_ns_config_global
_ns_config_server [ns_info server]

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
