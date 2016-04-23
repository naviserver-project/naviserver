#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://aolserver.com/.
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
# modules/nsperm/htaccess.tcl -
#   support for .htaccess files
#

proc ns_perm_filter { args } {

    set url [ns_conn url]

    # Do not allow to server special files
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
    if { $op != "allow" && $op != "deny" } {
        return
    }

    # Without users we clear the whole directory from any permissions
    if { [lindex $line 1] == "" } {
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
