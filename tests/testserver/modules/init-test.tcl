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

# init-test.tcl -
#
#     Support routines for the tests in tests/init.test
#

#
# A simple proc which should be replicated in all interps after startup.
#

proc testproc1 {} {
    ns_log notice testproc1
}

#
# A namespaced proc which should be replicated in all interps after startup.
#

namespace eval testnamespace {}

proc testnamespace::testproc2 {} {
    ns_log notice testproc1
}

#
# Variables in the global namespace should not get replicated.
#

set testglobalvariable 1

#
# Variables in namespaces should not get replicated?
#

set testnamespace::testvariable2 1

interp alias {} ::testalias1 {}                 testproc1
interp alias {} ::testnamespace::testalias2 {}  testnamespace::testproc2
interp alias {}   testalias11 {}                testproc1
interp alias {}   testnamespace::testalias12 {} testnamespace::testproc2
