#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://www.mozilla.org/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is AOLserver Code and related documentation
# distributed by AOL.
# 
# The Initial Developer of the Original Code is America Online,
# Inc. Portions created by AOL are Copyright (C) 1999 America Online,
# Inc. All Rights Reserved.
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
