#
# The contents of this file are subject to the AOLserver Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://aolserver.com/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# Copyright (C) 2005 Stephen Deasey <sdeasey@users.sf.net>
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
#
#
# $Header$
#


if {![string match testvhost* [ns_info server]]} {
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



if {![string equal testvhost2 [ns_info server]]} {
    return
}

ns_serverrootproc nstest_serverroot arg
ns_locationproc   nstest_location arg

proc nstest_serverroot {{host ""} args} {
    if {![string equal $host ""]} {
        set path [eval file join testserverroot $host $args]
    } else {
        set path [eval file join testserverroot $args]
    }
    return $path
}

proc nstest_location {args} {
    return "testlocation.$args"
}
