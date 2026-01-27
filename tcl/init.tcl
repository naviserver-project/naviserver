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
# init.tcl --
#
#    NaviServer looks for init.tcl before sourcing all other files
#    in directory order.
#

#
# Initialize errorCode and errorInfo like tclsh does.
#

set ::errorCode ""
set ::errorInfo ""

#
# Make sure Tcl package loader starts looking for
# packages with our private library directory and not
# in some public, like /usr/local/lib or such. This
# way we avoid clashes with modules having multiple
# versions, one for general use and one for NaviServer.
#

if {[info exists ::auto_path] == 0} {
    set ::auto_path [file join [ns_info home] lib]
} else {
    set ::auto_path [concat [file join [ns_info home] lib] $::auto_path]
}
#
# Allow environment variables (such as "oacs_httpport" or
# "oacs_ipaddress") to overload predefined variables specified via Tcl
# dict.
#
# If the configuration dict contains key "setupfile" and its
# configured value is non-empty, source it after environment-variable
# overrides have been applied.
#
# The setup file is intended for instance-specific variable assignments
# (no ns_section blocks).  When "setupfile" is a relative path, it is
# resolved relative to the config root derived from [ns_info config]
# (the -t argument):
#   - if -t is a directory:  <configdir>/<setupfile>
#   - if -t is a file:       <dirname(-t)>/<setupfile>
#
proc ns_configure_variables {prefix defaultConfig} {
    set builtins {
        argc argv auto_path defaultConfig env errorCode errorInfo optind
        tcl_library tcl_patchLevel tcl_pkgPath tcl_platform tcl_version}
    foreach var [uplevel {info vars}] {
        if {[uplevel [list array exists $var]]} continue
        dict set vars $var value  [uplevel [list set $var]]
        dict set vars $var source [expr {$var in $builtins ? "builtin" : "preset"}]
    }

    foreach var [dict keys $defaultConfig] {
        #
        # If the variable is already set in the script, take this
        # value.
        #
        if {[dict exists $vars $var value]} {
            dict set vars $var source "config file"
        } elseif {[info exists ::env(${prefix}$var)]} {
            #
            # If we have an environment variable for this variable set,
            # take it.
            #
            dict set vars $var value $::env(${prefix}$var)
            dict set vars $var source "environment variable"
        } else {
            #
            # Otherwise set the variable to the default from the dict.
            #
            dict set vars $var value [dict get $defaultConfig $var]
            dict set vars $var source "default configuration"
        }

        if {$var eq "setupfile"} {
            #
            # Keep setupfile local; still allow $substitutions.
            #
            set setupfile [uplevel [list subst [dict get $vars $var value]]]
            if {$setupfile ne ""} {
                ns_log notice "setting $var to '$setupfile' from [dict get $vars $var source]"
            }
        } else {
            #
            # Use "subst" to support $substitutions in the values.
            #
            set v [uplevel [list set $var [uplevel [list subst [dict get $vars $var value]]]]]
            #ns_log notice "setting $var to '$v' from $source"
            dict set vars $var value $v
        }
    }

    #
    # Optional instance setup file
    #

    if {[info exists setupfile] && $setupfile ne ""} {
        if {[file pathtype $setupfile] eq "absolute"} {
            set _setupPath [file normalize $setupfile]
        } else {
            set _cfg [ns_info config]
            if {$_cfg eq ""} {
                set _cfgRoot [pwd]
            } elseif {[file isdirectory $_cfg]} {
                set _cfgRoot $_cfg
            } else {
                set _cfgRoot [file dirname $_cfg]
            }
            set _cfgRoot [file normalize $_cfgRoot]
            set _setupPath [file normalize [file join $_cfgRoot $setupfile]]
        }

        if {![file exists $_setupPath]} {
            error "setupfile not found: $_setupPath"
        }

        ns_log notice "sourcing setup file: $_setupPath"
        uplevel [list source $_setupPath]


        foreach var [uplevel {info vars}] {
            if {[uplevel [list array exists $var]]} continue
            set v [uplevel [list set $var]]

            if {[dict exists $vars $var]} {
                # Known variable; update on change
                if {$v ne [dict get $vars $var value]} {
                    dict set vars $var value  $v
                    dict set vars $var source "setup file"
                }
            } else {
                # New variable introduced by setup file
                dict set vars $var value $v
                dict set vars $var source "setup file"
            }
        }
    }

    #
    # Report all variables with their sources
    #
    foreach var [lsort [dict keys $vars]] {
        if {$var eq "setupfile"} continue
        dict with vars $var {
            if {$source ne "builtin"} {
                ns_log notice "setting $var to '$value' from $source"
            }
        }
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
