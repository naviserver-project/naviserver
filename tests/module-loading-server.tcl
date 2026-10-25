# -*- Tcl -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Verify configured versus successfully loaded Tcl modules without listeners.
# tclsh tests/module-loading-server.tcl /path/to/nsd
set arguments $argv
set argv {}
package require tcltest 2.2
if {[llength $arguments] != 1} {error "usage: tclsh module-loading-server.tcl /path/to/nsd"}
set executable [file normalize [lindex $arguments 0]]
set root [file normalize [file join [file dirname [info script]] ..]]
set fixture [::tcltest::makeDirectory module-loading-server]
proc writeFile {path contents} {
    file mkdir [file dirname $path]
    set f [open $path w]
    puts $f $contents
    close $f
}
foreach name {init.tcl nstrace.tcl} {
    file mkdir [file join $fixture core]
    file copy -force [file join $root tcl $name] [file join $fixture core $name]
}
writeFile [file join $fixture core good procs.tcl] {proc good_module {} {return loaded}}
writeFile [file join $fixture core override procs.tcl] {error {shared file must be overridden}}
writeFile [file join $fixture modules override procs.tcl] {proc private_module {} {return loaded}}
writeFile [file join $fixture modules initonly init.tcl] {proc init_module {} {return loaded}}
writeFile [file join $fixture modules metadata init.tcl] {
    ns_module version 1.2.3
    ns_module tag loading-test
    nsv_set metadata duringInit [list [ns_module version] [ns_module tag]]
}
writeFile [file join $fixture modules failed init.tcl] {
    ns_module version 9.9
    error {intentional module initialization failure}
}
writeFile [file join $fixture modules broken procs.tcl] {error {intentional definition failure}}
file mkdir [file join $fixture modules empty]
writeFile [file join $fixture check.tcl] {
    proc assert {condition message} {
        if {![uplevel 1 [list expr $condition]]} {error $message}
    }
    set loaded [ns_server modules]
    foreach module {good override initonly metadata nslog} {
        assert {[dict exists $loaded $module]} "missing loaded module $module"
    }
    foreach module {missing empty failed broken} {
        assert {![dict exists $loaded $module]} "incorrectly loaded module $module"
        assert {$module in [ns_ictl getmodules]} "configured module disappeared: $module"
    }
    assert {[good_module] eq "loaded" && [private_module] eq "loaded" && [init_module] eq "loaded"} definitions
    assert {[nsv_get metadata duringInit] eq "1.2.3 loading-test"} startup_metadata
    assert {[dict get $loaded metadata version] eq "1.2.3" && [dict get $loaded metadata tag] eq "loading-test"} metadata
    assert {[dict get $loaded nslog type] eq "module"} native_module
    assert {[_ns_module_files initonly] eq ""} init_only_not_reloadable
    ns_module reload good
    assert {[dict exists [ns_server modules] good]} reload_preserves_registry
    puts "MODULE LOADING ASSERTIONS PASSED"
    ns_shutdown
}
set failed 0
foreach lazy {0 1} {
    set config "ns_section ns/parameters \{\n"
    append config "ns_param home [list $fixture]\n"
    append config "ns_param tcllibrary [list [file join $fixture core]]\n"
    append config "ns_param systemlog [list [file join $fixture server.log]]\n"
    append config "ns_param pidfile [list [file join $fixture nsd.pid]]\n\}\n"
    append config "ns_section ns/servers {ns_param loading loading}\n"
    append config "ns_section ns/server/loading/tcl \{\n"
    append config "ns_param initfile [list [file join $root nsd init.tcl]]\n"
    append config "ns_param library [list [file join $fixture modules]]\n"
    append config "ns_param lazyloader $lazy\n\}\n"
    append config "ns_section ns/server/loading/modules \{\n"
    foreach module {missing empty good override initonly metadata failed broken} {
        append config "ns_param $module tcl\n"
    }
    append config "ns_param nslog [list [file join $root nslog nslog]]\n\}\n"
    writeFile [file join $fixture config.tcl] $config
    if {[catch {
        exec $executable -c -t [file join $fixture config.tcl] \
            [file join $fixture check.tcl] 2>@1
    } output] || [string first "MODULE LOADING ASSERTIONS PASSED" $output] < 0} {
        puts stderr "lazyloader=$lazy FAILED:\n$output"
        set failed 1
    } else {
        foreach module {missing empty} {
            if {![string match "*Warning: Tcl module '$module' has no Tcl files*module not loaded*" $output]} {
                puts stderr "lazyloader=$lazy: missing warning for $module"
                set failed 1
            }
        }
        puts "lazyloader=$lazy: loaded registry, missing-file warnings, metadata and native modules passed"
    }
}
::tcltest::removeDirectory $fixture
exit $failed
