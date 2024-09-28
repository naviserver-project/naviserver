#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Copyright 2006 (C) Stephen Deasey <sdeasey@gmail.com>
#
#

#
# all.tcl --
#
#       This file contains a top-level script to run all of the tests.
#       Execute it by invoking "source all.tcl" when running nsd in
#       command mode in this directory.
#

package require Tcl 8.5-
package require tcltest 2.2
namespace import tcltest::*
configure {*}$argv -singleproc true -testdir [file dirname [info script]]


rename tcltest::test tcltest::__test

proc tcltest::test args {

    ns_log debug >->-> \
        [format "%-16s" "[lindex $args 0]:"] ([lindex $args 1])

    uplevel 1 tcltest::__test $args
}

runAllTests
