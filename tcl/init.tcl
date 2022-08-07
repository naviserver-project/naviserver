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
# "oacs_ipaddress") to overload predefined variables specified via
# Tcl dict.
#
proc ns_configure_variables {prefix defaultConfig} {

    foreach var [dict keys $defaultConfig] {
        #
        # If the variable is already set in the script, take this
        # value.
        #
        if {[uplevel [list info exists var]]} {
            continue
        }
        #
        # If we have an environment variable for this variable set,
        # take it.
        #
        if {[info exists ::env(${prefix}$var)]} {
            set value $::env(${prefix}$var)
            ns_log notice "setting $var to '$value' from environment variable"
        } elseif {[dict exists $defaultConfig $var]} {
            #
            # Otherwise set the variable to the default from the dict.
            #
            set value [dict get $defaultConfig $var]
            ns_log notice "setting $var to '$value' from default configuration"
        } else {
            continue
        }
        #
        # Use "subst" to support $substitutions in the values.
        #
        uplevel [list set $var [uplevel [list subst $value]]]
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
