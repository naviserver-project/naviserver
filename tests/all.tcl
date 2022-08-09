# -*- Tcl -*-
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
# all.tcl --
#
#       This file contains a top-level script to run all of the tests.
#       Execute it by invoking "source all.tcl" when running nsd in
#       command mode in this directory.
#

#
# Make sure, the testfile runs with an expected locale
#
set env(LANG) en_US.UTF-8
encoding system utf-8

package require Tcl 8.5
package require tcltest 2.2
namespace import tcltest::*
configure {*}$argv -singleproc true -testdir [file dirname [info script]]



rename tcltest::test tcltest::__test

proc tcltest::test args {

    ns_log dev >->-> \
        [format "%-16s" "[lindex $args 0]:"] ([string trim [lindex $args 1]])

    uplevel 1 tcltest::__test $args
}


# For temporary debugging, you can turn test files on/off here.  But
# for committing public changes, you should instead use the tcltest
# "-constraints" feature, NOT do it here:
if {$::tcl_platform(platform) eq "windows"} {
   #configure -verbose {pass skip start}
   ## Temporarily (and silently!) SKIP these for now:
   #configure -notfile [list]
   ## ONLY run these tests:
   #configure -file [list ns_thread.test]
}

#
# The trick with cleanupTestsHook works for Tcl 8.5 and newer.
# The more modern variant of this is to use
#
# The trick with cleanupTestsHook works for Tcl 8.5 and newer.
# The more modern variant of this is to use
#
#   set code [runAllTests]
#
# but requires Tcl 8.6.
#
proc tcltest::cleanupTestsHook {} {
    variable numTests
    upvar 2 testFileFailures crashed
    set ::code [expr {$numTests(Failed) > 0}]
    if {[info exists crashed]} {
        set ::code [expr {$::code || [llength $crashed]}]
    }
}

#ns_logctl severity Debug(ns:driver) true
#ns_logctl severity debug on

runAllTests

#
# The "notice" messages during test shutdown are typically not very
# interesting, so turn it off to make the output shorter.
#
ns_logctl severity notice off

#
# Shutdown the server to let the cleanup handlers run
#
#foreach s [ns_info servers] {puts stderr "$s: [ns_server -server $s stats]"}
ns_shutdown

#
# Wait until these are finished, ns_shutdown will terminate this script
#
vwait forever
exit $code
