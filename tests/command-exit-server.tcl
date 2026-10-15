# -*- Tcl -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Command-shell exit must leave process shutdown to the main thread.
# tclsh tests/command-exit-server.tcl /path/to/nsd
set arguments $argv
set argv {}
package require tcltest 2.2
namespace import ::tcltest::*
if {[llength $arguments] != 1} {error "usage: tclsh command-exit-server.tcl /path/to/nsd"}
set executable [file normalize [lindex $arguments 0]]
set root [file normalize [file join [file dirname [info script]] ..]]
set fixture [makeDirectory command-exit-server]
proc writeFile {path contents} {
    set f [open $path w]
    puts $f $contents
    close $f
}
set core [file join $fixture core]
file mkdir $core
foreach name {init.tcl nstrace.tcl} {
    file copy -force [file join $root tcl $name] [file join $core $name]
}
# A marker written by the main thread proves server shutdown completed before
# the process exited. Merely observing the expected exit status is insufficient.
writeFile [file join $core exit-test.tcl] {
    ns_atexit {
        set f [open [file join [ns_library shared] shutdown-complete] w]
        puts $f complete
        close $f
    }
}
set config "ns_section ns/parameters \{\n"
append config "ns_param home [list $fixture]\n"
append config "ns_param tcllibrary [list $core]\n"
append config "ns_param systemlog [list [file join $fixture server.log]]\n"
append config "ns_param pidfile [list [file join $fixture nsd.pid]]\n\}\n"
append config "ns_section ns/servers {ns_param exittest exittest}\n"
append config "ns_section ns/server/exittest/tcl \{\n"
append config "ns_param initfile [list [file join $root nsd init.tcl]]\n"
append config "ns_param library [list $core]\n\}\n"
writeFile [file join $fixture config.tcl] $config
foreach {name body expected} {
    eof {puts {script completed}} 0
    error {error expected_script_error} 1
    exit {exit 7} 7
    shutdown {ns_shutdown} 0
    restart {ns_shutdown -restart} 2
} {
    test command-exit-$name {script exit status and orderly server shutdown} -setup {
        file delete -force [file join $core shutdown-complete]
        writeFile [file join $fixture script.tcl] $body
    } -body {
        set status 0
        if {[catch {
            exec $executable -c -t [file join $fixture config.tcl] \
                [file join $fixture script.tcl] 2>@1
        } output options]} {
            set code [dict get $options -errorcode]
            if {[lindex $code 0] ne "CHILDSTATUS"} {error $output}
            set status [lindex $code 2]
        }
        list $status [file exists [file join $core shutdown-complete]]
    } -result [list $expected 1]
}
removeDirectory $fixture
set failed $::tcltest::numTests(Failed)
cleanupTests
exit [expr {$failed != 0}]
