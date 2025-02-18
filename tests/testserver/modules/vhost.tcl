# -*- Tcl -*-
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Copyright (C) 2005 Stephen Deasey <sdeasey@users.sf.net>
#
#

if {![string match "testvhost*" [ns_info server]]} {
    return
}

ns_register_proc GET /serverpath {
    set cmd [list ns_serverpath]
    if {[ns_queryexists host]} {
        lappend cmd -host [ns_queryget host] --
    } else {
        lappend cmd --
    }
    if {[ns_queryexists path]} {
        lappend cmd [ns_queryget path]
    }
    ns_return 200 text/plain [eval $cmd] ;#}


ns_register_proc GET /pagepath {
    set cmd [list ns_pagepath]
    if {[ns_queryexists host]} {
        lappend cmd -host [ns_queryget host] --
    } else {
        lappend cmd --
    }
    if {[ns_queryexists path]} {
        lappend cmd [ns_queryget path]
    }
    ns_return 200 text/plain [eval $cmd] ;#}


ns_register_proc GET /location {
    ns_return 200 text/plain [ns_conn location] ;#}



if {"testvhost2" ne [ns_info server] } {
    return
}

ns_serverrootproc nstest::serverroot arg
ns_locationproc   nstest::location arg

proc nstest::serverroot {{host ""} args} {
    if {$host ne "" } {
        set path [eval file join testserverroot $host $args]
    } else {
        set path [eval file join testserverroot $args]
    }
    return /$path
}

proc nstest::location {args} {
    return "testlocation.$args"
}
