# -*- Tcl -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# tclsh tests/core-reload-server.tcl /path/to/nsd
set arguments $argv
set argv {}
package require tcltest 2.2
if {[llength $arguments] != 1} {error "usage: tclsh core-reload-server.tcl /path/to/nsd"}
set executable [file normalize [lindex $arguments 0]]
set root [file normalize [file join [file dirname [info script]] ..]]
set fixture [::tcltest::makeDirectory core-reload-server]
proc writeFile {path contents} {
    file mkdir [file dirname $path]
    set f [open $path w]; puts $f $contents; close $f
}
foreach name {init.tcl nstrace.tcl} {
    file mkdir [file join $fixture core]
    file copy -force [file join $root tcl $name] [file join $fixture core $name]
}
writeFile [file join $fixture core util.tcl] {error {private override must win}}
writeFile [file join $fixture core http.tcl] {
    ns_module require ./util.tcl util.tcl
    proc core_http {} [list return [core_api]]
}
writeFile [file join $fixture core cache.tcl] {proc startup_only {} {return present}}
writeFile [file join $fixture modules a init.tcl] {ns_module version 1.0}
writeFile [file join $fixture modules a ext.tcl] {
    ns_module require -core ./http.tcl http.tcl
    proc core_extension {} [list return [core_http]]
}
writeFile [file join $fixture check.tcl] {
    proc assert {condition message} {
        if {![uplevel 1 [list expr $condition]]} {error $message}
    }
    assert {[core_extension] eq "old"} startup
    set inventory [ns_module files -core]
    assert {[dict get $inventory util.tcl path] eq [file join [ns_library private] util.tcl]} private_override
    foreach name {init.tcl nstrace.tcl nsd/init.tcl cache.tcl} {
        assert {![dict get $inventory $name reloadable]} "bootstrap eligibility: $name"
    }
    assert {![dict get [ns_module files a] init.tcl reloadable]} module_init
    set f [open [dict get $inventory util.tcl path] w]
    puts $f {proc core_api {} {return new}}
    close $f
    set affected [ns_module reload -core .//util.tcl util.tcl]
    assert {[llength $affected] == 3 && [core_extension] eq "new"} dependent_reload
    assert {[dict get [ns_server modules] a version] eq "1.0"} metadata
    set q [ns_job create core-check 1]
    set result [ns_job wait $q [ns_job queue $q {
        list [core_extension] [dict get [ns_module files -core] util.tcl reloadable]
    }]]
    assert {$result eq "new 1"} propagation
    set epoch [ns_ictl epoch]
    assert {[catch {ns_module reload -core nstrace.tcl}]} bootstrap_rejected
    assert {[ns_ictl epoch] == $epoch} no_bootstrap_publication
    set f [open [dict get $inventory util.tcl path] w]
    puts $f {error broken}
    close $f
    assert {[catch {ns_module reload -core util.tcl} message] && $message eq "broken"} failed_source
    assert {[ns_ictl epoch] == $epoch} no_failed_publication
    puts "CORE RELOAD ASSERTIONS PASSED"
    ns_shutdown
}
set failed 0
foreach lazy {0 1} {
    writeFile [file join $fixture modules util.tcl] {proc core_api {} {return old}}
    set config "ns_section ns/parameters \{\n"
    append config "ns_param home [list $fixture]\n"
    append config "ns_param tcllibrary [list [file join $fixture core]]\n"
    append config "ns_param systemlog [list [file join $fixture server.log]]\n"
    append config "ns_param pidfile [list [file join $fixture nsd.pid]]\n\}\n"
    append config "ns_section ns/servers {ns_param coretest coretest}\n"
    append config "ns_section ns/server/coretest/tcl \{\n"
    append config "ns_param initfile [list [file join $root nsd init.tcl]]\n"
    append config "ns_param library [list [file join $fixture modules]]\n"
    append config "ns_param lazyloader $lazy\n\}\n"
    append config "ns_section ns/server/coretest/modules {ns_param a tcl}\n"
    writeFile [file join $fixture config.tcl] $config
    if {[catch {
        exec $executable -c -t [file join $fixture config.tcl] \
            [file join $fixture check.tcl] 2>@1
    } output] || [string first "CORE RELOAD ASSERTIONS PASSED" $output] < 0} {
        puts stderr "lazyloader=$lazy FAILED:\n$output"
        set failed 1
    } else {
        puts "lazyloader=$lazy: core inventory, overrides, cross-module propagation and exclusions passed"
    }
}
::tcltest::removeDirectory $fixture
exit $failed
