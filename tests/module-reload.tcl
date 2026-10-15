# -*- Tcl -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Standalone loader tests: tclsh tests/module-reload.tcl
package require tcltest 2.2
namespace import ::tcltest::*
set root [file normalize [file join [file dirname [info script]] ..]]
set f [open [file join $root nsd init.tcl]]
set source [read $f]
close $f
# Exercise the actual loader and dispatcher without booting a server.
set start [string first {namespace eval ::ns_module} $source]
set end [string first {proc _ns_load_server_modules} $source]
eval [string range $source $start $end-1]
set start [string first {proc ns_module } $source]
set definition ""
foreach line [split [string range $source $start end] \n] {
    append definition $line \n
    if {[info complete $definition]} {break}
}
eval $definition
set fixture [makeDirectory module-reload]
proc ns_library {kind module} {return [file join $::fixture $kind $module]}
proc ns_ictl {args} {return {a b c}}
proc ns_eval {option args} {
    incr ::evaluations
    uplevel #0 $args
}
proc writeModule {module name body {kind shared}} {
    set directory [ns_library $kind $module]
    file mkdir [file dirname [file join $directory $name]]
    set f [open [file join $directory $name] w]
    puts $f $body
    close $f
}
proc reset {} {
    set ::ns_module::dependencies [dict create]
    set ::ns_module::loaded [dict create]
    set ::ns_module::stack {}
    set ::events {}
    set ::evaluations 0
    unset -nocomplain ::_module
    file delete -force $::fixture
    file mkdir $::fixture
}
test module-1 {recursive require, context restoration, and load once} -setup {
    reset
    writeModule a base.tcl {lappend ::events a; proc base {} {return old}}
    writeModule b ext.tcl {
        ns_module require -module a base.tcl
        lappend ::events [ns_module name]
        proc extension {} {return [base]}
    }
} -body {
    _ns_module_source {b ext.tcl}
    _ns_module_source {a base.tcl}
    list $::events [extension] [ns_module name]
} -result {{a b} old {}}
test module-2 {module reload restores cross-module dependents transitively} -setup {
    writeModule c leaf.tcl {
        ns_module require -module b ext.tcl
        lappend ::events c
        proc leaf {} {return [extension]}
    }
    _ns_module_source {c leaf.tcl}
    writeModule a base.tcl {
        lappend ::events a2
        rename extension {}
        proc base {} {return new}
    }
    set ::events {}
} -body {
    ns_module reload a
    list $::events [leaf] $::evaluations
} -result {{a2 b c} new 1}
test module-3 {multiple local files, whole module, private override} -setup {
    reset
    writeModule a one.tcl {lappend ::events shared}
    writeModule a one.tcl {lappend ::events private} private
    writeModule a two.tcl {ns_module require one.tcl; lappend ::events two}
    writeModule a init.tcl {error {init must not be reloaded}}
    writeModule b ext.tcl {ns_module require -module a; lappend ::events b}
    writeModule b extra.tcl {lappend ::events extra}
    writeModule b main.tcl {ns_module require ext.tcl extra.tcl; lappend ::events main}
} -body {
    _ns_module_source {b main.tcl}
    list $::events [lsort [dict keys $::ns_module::loaded]]
} -result {{private two b extra main} {{a one.tcl} {a two.tcl} {b ext.tcl} {b extra.tcl} {b main.tcl}}}
test module-4 {cycle reports a dependency chain and restores context} -setup {
    reset
    writeModule a a.tcl {ns_module require -module b b.tcl}
    writeModule b b.tcl {ns_module require -module a a.tcl}
} -body {
    set code [catch {_ns_module_source {a a.tcl}} message]
    list $code $message $::ns_module::stack [ns_module name] $::ns_module::loaded
} -result {1 {module dependency cycle: a a.tcl -> b b.tcl -> a a.tcl} {} {} {}}
test module-5 {failed reload restores registry and propagates error} -setup {
    reset
    writeModule a a.tcl {proc example {} {return ok}}
    _ns_module_source {a a.tcl}
    set before [list $::ns_module::loaded $::ns_module::dependencies]
    writeModule a a.tcl {error broken}
} -body {
    list [catch {ns_module reload a} message] $message \
        [expr {$before eq [list $::ns_module::loaded $::ns_module::dependencies]}] \
        $::ns_module::stack
} -result {1 broken 1 {}}
test module-6 {removed files fail before any re-execution} -setup {
    reset
    writeModule a a.tcl {lappend ::events a}
    _ns_module_source {a a.tcl}
    file delete [file join [ns_library shared a] a.tcl]
    set ::events {}
} -body {
    list [catch {ns_module reload a} message] $message $::events
} -result {1 {module file not found: a/a.tcl} {}}
test module-7 {dependency declarations are replaced on successful reload} -setup {
    reset
    writeModule a a.tcl {lappend ::events a}
    writeModule b b.tcl {ns_module require -module a a.tcl; lappend ::events b}
    _ns_module_source {b b.tcl}
    writeModule b b.tcl {lappend ::events b}
    ns_module reload b
    set ::events {}
} -body {
    ns_module reload a
    set ::events
} -result a
test module-8 {legacy ns_module context interface} -body {
    ns_module name example
    ns_module library /private/example
    set result [list [ns_module name] [ns_module private] [ns_module network nssock]]
    ns_module clear
    lappend result [ns_module name]
} -result {example /private/example 1 {}}
test module-9 {diamond dependencies execute once in prerequisite order} -setup {
    reset
    writeModule a base.tcl {lappend ::events base}
    writeModule b left.tcl {ns_module require -module a base.tcl; lappend ::events left}
    writeModule b right.tcl {ns_module require -module a base.tcl; lappend ::events right}
    writeModule c leaf.tcl {ns_module require -module b left.tcl right.tcl; lappend ::events leaf}
    _ns_module_source {c leaf.tcl}
    set ::events {}
} -body {
    ns_module reload a
    set ::events
} -result {base left right leaf}
test module-10 {failed nested load restores all dependency bookkeeping} -setup {
    reset
    writeModule a base.tcl {proc example {} {return ok}}
    writeModule b broken.tcl {ns_module require -module a base.tcl; error broken}
} -body {
    catch {_ns_module_source {b broken.tcl}}
    list $::ns_module::loaded $::ns_module::dependencies $::ns_module::stack
} -result {{} {} {}}
test module-11 {relative aliases load and reload an overridden file once} -setup {
    reset
    writeModule a base.tcl {error {shared file must not override private}}
    writeModule a base.tcl {lappend ::events base} private
    writeModule b ext.tcl {
        ns_module require -module a ./base.tcl .//base.tcl base.tcl
        lappend ::events ext
    }
} -body {
    _ns_module_source {b .//ext.tcl}
    _ns_module_source {a base.tcl}
    set startup $::events
    set ::events {}
    ns_module reload a
    list $startup $::events [lsort [dict keys $::ns_module::loaded]] \
        [dict get $::ns_module::dependencies {b ext.tcl}]
} -result {{base ext} {base ext} {{a base.tcl} {b ext.tcl}} {{a base.tcl}}}
test module-12 {nested local aliases share a canonical dependency identity} -setup {
    reset
    writeModule a base.tcl {lappend ::events base}
    writeModule b sub/helper.tcl {
        ns_module require -module a ./base.tcl
        lappend ::events helper
    }
    writeModule b main.tcl {
        ns_module require sub//./helper.tcl ./sub/helper.tcl
        lappend ::events main
    }
    _ns_module_source {b main.tcl}
    set ::events {}
} -body {
    ns_module reload a
    list $::events [dict get $::ns_module::dependencies {b main.tcl}]
} -result {{base helper main} {{b sub/helper.tcl}}}
test module-13 {relative aliases cannot hide a dependency cycle} -setup {
    reset
    writeModule a base.tcl {ns_module require ./base.tcl}
} -body {
    list [catch {_ns_module_source {a .//base.tcl}} message] $message \
        $::ns_module::loaded $::ns_module::stack
} -result {1 {module dependency cycle: a base.tcl -> a base.tcl} {} {}}
test module-14 {normalization retains rejection of invalid module filenames} -body {
    set results {}
    foreach name [list ../base.tcl sub/../base.tcl ./init.tcl \
                      sub/./init.tcl ./base.txt [file join $fixture base.tcl]] {
        lappend results [catch {_ns_module_key a $name}]
    }
    set results
} -result {1 1 1 1 1 1}
removeDirectory $fixture
set failures $::tcltest::numTests(Failed)
cleanupTests
exit [expr {$failures != 0}]
