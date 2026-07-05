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
#ns_log notice "DEBUG all.tcl"

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
set ::env(LANG) en_US.UTF-8
encoding system utf-8

package require tcltest 2.2
namespace import tcltest::*
configure {*}$argv -singleproc true -testdir [file dirname [info script]]

set verboseTest 0

if {$verboseTest} {
    rename tcltest::test tcltest::__test
    proc tcltest::test args {
        ns_log dev >->-> \
            [format "%-16s" "[lindex $args 0]:"] ([string trim [lindex $args 1]])
        uplevel 1 tcltest::__test $args
    }
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

ns_logctl severity Debug(memory) on

proc tcltest::ns_test_meminfo_pick {dict keys} {
    set result [dict create]
    foreach key $keys {
        if {[dict exists $dict $key]} {
            dict set result $key [dict get $dict $key]
        }
    }
    return $result
}

proc tcltest::ns_test_meminfo_snapshot {where} {
    try {
        set meminfo [ns_info meminfo]
    } on error {errorMsg} {
        ns_log notice "test-meminfo $where: ERROR $errorMsg"
        return
    }
    #puts stderr "DEBUG: $meminfo"
    if {![dict exists $meminfo stats] || [dict get $meminfo stats] eq ""} {
        #
        # No memory statistics available.  The memory statistics
        # require to run with SYSTEM_MALLOC and tcmalloc loaded with
        # LD_PREOAD (Linux) or DYLD_INSERT_LIBRARIES (macOS).
        #
        return
    } else {
        set picked [tcltest::ns_test_meminfo_pick $meminfo {
            current_allocated_bytes
            heap_size
            pageheap_free_bytes
            pageheap_unmapped_bytes
            central_cache_free_bytes
            transfer_cache_free_bytes
            thread_cache_free_bytes
            current_total_thread_cache_bytes
        }]

        ns_log Debug(memory) "test-meminfo $where: $picked"
    }
}

#ns_logctl severity Debug(ns:driver) true
#ns_logctl severity debug on

if {"start" in [configure -verbose]} {
    ns_logctl severity notice on
    ns_logctl severity warning on
}

tcltest::ns_test_meminfo_snapshot "before runAllTests"

runAllTests

tcltest::ns_test_meminfo_snapshot "after runAllTests"
#
# The "notice" messages during test shutdown are typically not very
# interesting, so turn it off to make the output shorter.
#
ns_logctl severity notice off

#
# Shutdown the server to let the cleanup handlers run
#
#foreach s [ns_info servers] {puts stderr "$s: [ns_server -server $s stats]"}

if {$code ne "0"} {
    #
    # We had some errors during the regression test, force a nonzero
    # exit code
    #
    ns_shutdown -restart
} else {
    ns_shutdown
}
tcltest::ns_test_meminfo_snapshot "shutdown requested"

#
# Wait until these are finished, ns_shutdown will terminate this script
#
vwait forever
