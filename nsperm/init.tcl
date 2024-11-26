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
# modules/nsperm/init.tcl -
#   Initialization for nsperm module
#

proc init_nsperm { } {
    set dir [file join [ns_info home] modules nsperm]

    #
    # Parse hosts.allow
    #
    set filename [file join $dir hosts.allow]
    if {[catch {set file [open $filename r]} ignore] == 0} {
        while {![eof $file]} {
            set line [gets $file]
            if {[string index $line 0] != "#"} {
                if {$line ne ""} {
                    set pos [string first : $line]
                    if {$pos < 0} {
                        ns_log error "init_nsperm: bad line in $filename: $line"
                    } else {
                        set user [string trim [string range $line 0 $pos-1]]
                        set addrs [string trim [string range $line $pos+1 end]]
                        foreach addr [split $addrs ,] {
                            set addr [string trim $addr]
                            lappend _ns_allow($user) $addr
                        }
                        ns_log notice "... user <$user> addrs <$addrs> --> $_ns_allow($user)"
                    }
                }
            }
        }
        close $file
    }

    #
    # Parse hosts.deny
    #
    set filename [file join $dir hosts.deny]
    if {[catch {set file [open $filename r]} ignore] == 0} {
        while {![eof $file]} {
            set line [gets $file]
            if {[string index $line 0] != "#"} {
                if {$line ne ""} {
                    set pos [string first : $line]
                    if {$pos < 0} {
                        ns_log error "init_nsperm: bad line in $filename: $line"
                    } else {
                        set user [string trim [string range $line 0 $pos-1]]
                        set addrs [string trim [string range $line $pos+1 end]]
                        foreach addr [split $addrs ,] {
                            set addr [string trim $addr]
                            if {[info exists _ns_allow($user)]} {
                                ns_log error "init_nsperm: both allow and deny entries exist for user \"$user\""
                            } else {
                                lappend _ns_deny($user) $addr
                            }
                        }
                    }
                }
            }
        }
        close $file
    }

    #
    # Parse passwd
    #
    set filename [file join $dir passwd]
    if {[catch {set file [open $filename r]} ignore] == 0} {
        while {![eof $file]} {
            set line [gets $file]
            if {[string index $line 0] != "#"} {
                if {$line ne ""} {
                    set list [split $line :]
                    if {[llength $list] != 7} {
                        ns_log error "nsperm_init: bad line in $filename: $line"
                    } else {
                        set flag ""
                        # Treat "" as empty string
                        set user [string trim [lindex $list 0] {""}]
                        set pass [lindex $list 1]
                        set uf1 [lindex $list 4]
                        set params "[list $user] [list $pass] [list $uf1]"
                        if {[info exists _ns_allow($user)]} {
                            set flag "-allow"
                            foreach a $_ns_allow($user) {
                                append params " [list $a]"
                            }
                        }
                        if {[info exists _ns_deny($user)]} {
                            set flag "-deny"
                            foreach a $_ns_deny($user) {
                                append params " [list $a]"
                            }
                        }
                        ns_log notice "PASSWD call <ns_perm adduser $flag $params>"
                        if {[catch { ns_perm adduser {*}$flag {*}$params } errmsg]} {
                            ns_log Error init_nsperm: $errmsg
                        }
                    }
                }
            }
        }
        close $file
    }

    #
    # Parse group
    #
    set filename [file join $dir group]
    if {[catch {set file [open $filename r]} ignore] == 0} {
        while {![eof $file]} {
            set line [gets $file]
            if {[string index $line 0] != "#"} {
                if {$line ne ""} {
                    set list [split $line :]
                    if {[llength $list] != 4} {
                        ns_log error "nsperm_init: bad line in $filename: $line"
                    } else {
                        # Treat "" as empty name
                        set group [string trim [lindex $list 0] {""}]
                        set users [split [lindex $list 3] ,]
                        set cmd "ns_perm addgroup [list $group]"
                        foreach user $users {
                            set user [string trim [string trim $user] {""}]
                            append cmd " [list $user]"
                        }
                        if {[catch { eval $cmd } errmsg]} {
                            ns_log Error init_nsperm: $errmsg
                        }
                    }
                }
            }
        }
        close $file
    }

    #
    # Parse perms
    #
    set filename [file join $dir perms]
    if {[catch {set file [open $filename r]} ignore] == 0} {
        while {![eof $file]} {
            set line [gets $file]
            if {[string index $line 0] != "#"} {
                if {$line ne ""} {
                    if {[llength $line] != 5} {
                        ns_log error "nsperm_init: bad line in $filename: $line"
                    } else {
                        set action [lindex $line 0]
                        set inherit [lindex $line 1]
                        set method [lindex $line 2]
                        set url [lindex $line 3]
                        set entity [lindex $line 4]
                        set cmd "ns_perm [list $action]"
                        if {$inherit eq "noinherit"} {
                            append cmd " -noinherit"
                        }
                        append cmd " [list $method] [list $url] [list $entity]"
                        if {[catch { eval $cmd } errmsg]} {
                            ns_log Error init_nsperm: $errmsg
                        }
                    }
                }
            }
        }
        close $file
    }
}

#
# ns_permpasswd lets you set a password in the nsperm passwd file.
# It is implemented in Tcl because the passwd file is no inherently a
# part of the nsperm module--just a nice interface provided by the
# supporting Tcl code.
#
# oldpass must either be the user's old password or nsadmin's password
# for the action to succeed.
#

proc ns_permpasswd { user oldpasswd newpasswd } {

    set dir [file join [ns_info home] modules nsperm]
    set filename [file join $dir passwd]
    set file [open $filename r]
    set oldfile ""

    #
    # Verify that this is an allowed action
    #

    if {[catch {ns_perm checkpass $user $oldpasswd} ignore] != 0} {
        if {[catch {ns_perm checkpass nsadmin $oldpasswd} ignore] != 0} {
            return "incorrect old password"
        }
    }

    while {![eof $file]} {
        set line [gets $file]
        set entryLine $line
        if {[string index $line 0] != "#"} {
            if {$line ne ""} {
                set list [split $line :]
                if {[llength $list] != 7} {
                    ns_log error "ns_permpassword: bad line in $filename: $line"
                } else {
                    set u [lindex $list 0]
                    if {$u eq $user} {
                        set entryLine "[lindex $list 0]:[ns_crypt $newpasswd CU]:[lindex $list 2]:[lindex $list 3]:[lindex $list 4]:[lindex $list 5]:[lindex $list 6]"
                    }
                }
            }
        }
        lappend oldfile $entryLine
    }
    close $file

    set file [open $filename w]
    foreach l $oldfile {
        puts $file $l
    }
    close $file

    ns_perm setpass $user [ns_crypt $newpasswd CU]
    return ""
}

proc ns_permreload {} {

    foreach { group u } [ns_perm listgroup] {
        ns_perm delgroup $group
    }
    foreach { user p d } [ns_perm listuser] {
        ns_perm deluser $user
    }
    foreach perm [ns_perm listperm] {
        ns_perm delperm [lindex $perm 0] [lindex $perm 1]
    }
    init_nsperm
}

#
# Initialize the module
#

init_nsperm

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
