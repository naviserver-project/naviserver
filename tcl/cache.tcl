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
# cache.tcl --
#
#   Simple cache for procs and commands.
#

set path "ns/server/[ns_info server]/tcl"
ns_cache_create ns:memoize \
    [ns_config -int -set $path memoizecache [expr {1024*1024*10}]]

#
# ns_memoize --
#
#   Evaluate the given script or proc and cache the result. Future calls
#   will return the cached value (if not expired).
#
#   See ns_cache_eval for details.
#
# Results:
#   Result of evaluating script.
#
# Side effects:
#   See ns_cache_eval.
#

proc ns_memoize {args} {
    ns_parseargs {{-timeout ""} {-expires ""} -- script args} $args

    if {$timeout ne ""} {
        set timeout "-timeout $timeout"
    }
    if {$expires ne ""} {
        set expires "-expires $expires"
    }
    set key [concat $script $args]
    eval ns_cache_eval $timeout $expires -- \
        ns:memoize [list $key] $script $args
}


#
# ns_memoize_flush --
#
#    Flush memoized results which match given glob pattern.
#
# Results:
#   Number of results flushed.
#
# Side effects:
#   None.
#

proc ns_memoize_flush {{pattern ""}} {
    if {$pattern eq ""} {
        return [ns_cache_flush ns:memoize]
    } else {
        return [ns_cache_flush -glob -- ns:memoize $pattern]
    }
}


#
# ns_memoize_stats --
#
#    Returns the stats for the memoize cache.
#
# Results:
#   List of stats in array get format.
#
# Side effects:
#   None.
#

proc ns_memoize_stats {} {
    return [ns_cache_stats ns:memoize]
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
