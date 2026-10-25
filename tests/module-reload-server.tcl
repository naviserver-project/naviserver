# -*- Tcl -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Live integration test, without network listeners:
# tclsh tests/module-reload-server.tcl /path/to/nsd ?--objects?
# --objects additionally requires NX, XOTcl 2, and nx::serializer.
# Child exit failures, including shutdown crashes, remain test failures even
# when all reload assertions completed successfully.
set arguments $argv
set argv {}
package require tcltest 2.2
if {[llength $arguments] ni {1 2}
    || ([llength $arguments] == 2 && [lindex $arguments 1] ne "--objects")} {
    error "usage: tclsh module-reload-server.tcl /path/to/nsd ?--objects?"
}
set executable [file normalize [lindex $arguments 0]]
set root [file normalize [file join [file dirname [info script]] ..]]
set fixture [::tcltest::makeDirectory module-reload-server]
proc writeFile {path contents} {
    file mkdir [file dirname $path]
    set f [open $path w]
    puts $f $contents
    close $f
}
# The plain Tcl cases use only core scripts and require no listeners.
foreach name {init.tcl nstrace.tcl} {
    file mkdir [file join $fixture core]
    file copy -force [file join $root tcl $name] [file join $fixture core $name]
}
writeFile [file join $fixture modules b ext.tcl] {
    ns_module require -module a ./base.tcl base.tcl
    proc module_ext {} [list return [module_base]]
}
writeFile [file join $fixture modules c leaf.tcl] {
    ns_module require -module b ext.tcl
    proc module_leaf {} [list return [module_ext]]
}
writeFile [file join $fixture check.tcl] {
    proc assert {condition message} {
        if {![uplevel 1 [list expr $condition]]} {error $message}
    }
    assert {[module_leaf] eq "old"} startup
    if {[info commands objectResults] ne ""} {
        assert {[objectResults] eq "child:nx:old child:xo:old"} objects_startup
        # Demonstrate that class recreation removes the externally defined methods.
        # Reload must repair this even though the dependency graph is unchanged.
        nx::Class create ::ReloadNXBase
        xotcl::Class create ::ReloadXOBase
        ::ReloadNXBase create ::ReloadNXProbe
        ::ReloadXOBase create ::ReloadXOProbe
        assert {[catch {::ReloadNXProbe extension}]} nx_method_removed
        assert {[catch {::ReloadXOProbe extension}]} xo_method_removed
        if {[info commands ::ReloadNXProbe] ne ""} {::ReloadNXProbe destroy}
        if {[info commands ::ReloadXOProbe] ne ""} {::ReloadXOProbe destroy}
        # Make subclass definitions observably stale too: reloading just the
        # method extension file must not be enough for this test to pass.
        ::ReloadNXChild public method child {} {return stale}
        ::ReloadXOChild instproc child {} {return stale}
        set classes [file join [ns_library private a] classes.tcl]
        set f [open $classes w]
        puts $f {
            nx::Class create ::ReloadNXBase {
                :public method version {} {return new}
            }
            xotcl::Class create ::ReloadXOBase
            ::ReloadXOBase instproc version {} {return new}
        }
        close $f
    }
    set base [file join [ns_library private a] base.tcl]
    set f [open $base w]
    puts $f {proc module_base {} {return new}}
    close $f
    ns_module reload a
    assert {[module_leaf] eq "new"} dependent_reload
    if {[info commands objectResults] ne ""} {
        assert {[objectResults] eq "child:nx:new child:xo:new"} objects_restored
    }
    set q [ns_job create module-check 1]
    set result [ns_job wait $q [ns_job queue $q {
        set moduleCount 0
        foreach key [dict keys $::ns_module::loaded] {
            if {[lindex $key 0] ne ""} {incr moduleCount}
        }
        list [module_leaf] $moduleCount \
            [expr {[info commands objectResults] eq "" ? {} : [objectResults]}]
    }]]
    if {[info commands objectResults] eq ""} {
        assert {$result eq "new 3 {}"} propagation
    } else {
        assert {$result eq "new 6 {child:nx:new child:xo:new}"} objects_propagation
        puts "NX/XOTCL RELOAD ASSERTIONS PASSED"
    }
    set epoch [ns_ictl epoch]
    set f [open $base w]
    puts $f {error broken}
    close $f
    assert {[catch {ns_module reload a} message] && $message eq "broken"} error
    assert {[ns_ictl epoch] == $epoch} failed_publication
    puts "MODULE RELOAD INTEGRATION PASSED"
    ns_shutdown
}
set failed 0
# Object serialization is provided by statescript (lazyloader=0). The
# trace-based lazy loader does not serialize NX/XOTcl objects at startup.
set cases {0 0 1 0}
if {[lindex $arguments 1] eq "--objects"} {lappend cases 0 1}
foreach {lazy objects} $cases {
    if {$objects} {
        # Load both object systems before module definitions.
        writeFile [file join $fixture core objects.tcl] {
            proc objectResults {} {
                set nx [::ReloadNXChild new]
                set xo [::ReloadXOChild new]
                set result [list [$nx child] [$xo child]]
                $nx destroy
                $xo destroy
                return $result
            }
            package require nx
            package require XOTcl 2
            package require nx::serializer
            namespace import -force ::nx::serializer::Serializer
        }
        writeFile [file join $fixture modules a classes.tcl] {
            nx::Class create ::ReloadNXBase {
                :public method version {} {return old}
            }
            xotcl::Class create ::ReloadXOBase
            ::ReloadXOBase instproc version {} {return old}
        }
        writeFile [file join $fixture modules b methods.tcl] {
            ns_module require -module a ./classes.tcl classes.tcl
            ::ReloadNXBase public method extension {} {return "nx:[:version]"}
            ::ReloadXOBase instproc extension {} {return "xo:[my version]"}
        }
        writeFile [file join $fixture modules b subclasses.tcl] {
            ns_module require ./methods.tcl methods.tcl
            nx::Class create ::ReloadNXChild -superclass ::ReloadNXBase {
                :public method child {} {return "child:[:extension]"}
            }
            xotcl::Class create ::ReloadXOChild -superclass ::ReloadXOBase
            ::ReloadXOChild instproc child {} {return "child:[my extension]"}
        }
    }
    writeFile [file join $fixture modules a base.tcl] {proc module_base {} {return old}}
    set config "ns_section ns/parameters \{\n"
    append config "ns_param home [list $fixture]\n"
    append config "ns_param tcllibrary [list [file join $fixture core]]\n"
    append config "ns_param systemlog [list [file join $fixture server.log]]\n"
    append config "ns_param pidfile [list [file join $fixture nsd.pid]]\n\}\n"
    append config "ns_section ns/servers {ns_param reloadtest reloadtest}\n"
    append config "ns_section ns/server/reloadtest/tcl \{\n"
    append config "ns_param initfile [list [file join $root nsd init.tcl]]\n"
    append config "ns_param library [list [file join $fixture modules]]\n"
    append config "ns_param lazyloader $lazy\n\}\n"
    append config "ns_section ns/server/reloadtest/modules {\n"
    append config "ns_param c tcl\nns_param b tcl\nns_param a tcl\n}\n"
    writeFile [file join $fixture config.tcl] $config
    if {[catch {
        exec $executable -c -t [file join $fixture config.tcl] \
            [file join $fixture check.tcl] 2>@1
    } output] || [string first "MODULE RELOAD INTEGRATION PASSED" $output] < 0} {
        puts stderr "lazyloader=$lazy objects=$objects FAILED:\n$output"
        set failed 1
    } else {
        puts "lazyloader=$lazy objects=$objects: startup, dependent reload, propagation, failed publication passed"
    }
}
::tcltest::removeDirectory $fixture
exit $failed
