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
# modules/nsperm/htaccess.tcl -
#   support for .htaccess files
#

proc ns_perm_filter { args } {

    set url [ns_conn url]

    # Do not serve special files
    switch -- [file tail $url] {
        .htaccess -
        .htpasswd {
            returnnotfound
            return filter_return
        }
    }

    set lock [nsv_get nsperm lock]

    set dir [ns_url2file $url]
    if { [file isdirectory $dir] == 0 } {
        set dir [file dirname $dir]
        set url [file dirname $url]
    }

    ns_mutex eval $lock {

        # Load passwd file if changed
        ns_perm_load [nsv_get nsperm passwdfile] $url ns_perm_adduser

        # Load access file if changed
        ns_perm_load $dir/.htaccess $url ns_perm_addperm
    }

    return filter_ok
}

proc ns_perm_load { file url callback } {

    if { [file exists $file] == 0 } {
        return
    }

    set mtime [file mtime $file]
    if { [nsv_exists nsperm $file] } {
        if { $mtime <= [nsv_get nsperm $file] } {
            return
        }
    }

    ns_log Notice ns_perm_load: $file: $url $callback

    if { [catch {
        foreach line [split [ns_fileread $file] "\n"] {
            $callback $file $url [split $line " :"]
        }
    } errmsg] } {
        ns_log Error ns_perm_load: $file: $errmsg
    }

    nsv_set nsperm $file $mtime
}

proc ns_perm_adduser { file url line } {
    
    ns_log debug "--- ns_perm_adduser [list $file $url $line]"

    if { [llength $line] < 2 } {
        return
    }

    set clear ""
    set user [lindex $line 0]
    set passwd [lindex $line 1]

    if { $user eq "" || $passwd eq "" } {
        return
    }

    ns_perm deluser $user

    if { [string range $passwd 0 1] ne "CU" } {
        ns_perm adduser -clear $user $passwd ""
    } else {
        ns_perm adduser $user $passwd ""
    }
}

proc ns_perm_addperm { file url line } {
    ns_log debug "--- ns_perm_addperm [list $file $url $line]"

    set op [lindex $line 0]
    if { $op ne "allow" && $op ne "deny" } {
        return
    }

    # Without users we clear the whole directory from any permissions
    if { [lindex $line 1] eq "" } {
        ns_perm delperm GET $url
        ns_perm delperm POST $url
    }

    ns_perm ${op}user GET $url {*}[lrange $line 1 end]
    ns_perm ${op}user POST $url {*}[lrange $line 1 end]

    # Make sure empty user is not allowed, default passwd contains
    # user with no name to support implicit allow mode (Why!?)
    ns_perm denyuser GET $url ""
    ns_perm denyuser POST $url ""
}

ns_runonce {

    set path ns/server/[ns_info server]/module/nsperm

    if { [ns_config -bool -set $path htaccess 0] } {
        nsv_set nsperm lock [ns_mutex create]
        nsv_set nsperm passwdfile [ns_config -set $path passwdfile [file join [ns_info home] modules nsperm passwd]]
        ns_register_filter preauth GET /* ns_perm_filter

        ns_log Notice "nsperm: enabling .htaccess support"
    }

}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
