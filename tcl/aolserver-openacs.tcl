#
# This is a small compatibility layer for OpenACS when used with 
# naviserver 4.99.3 or newer.  
#
# WARNING: The procs defined in this file are not intended to be a
# fully aolserver 4.* compliant implementation of these commands, but
# are intended only for the functionality used in OpenACS currently.
#
# The script implements 3 commands:
#  * ns_share (obsolete, but called from OpenACS)
#  * ns_cache (the naviserver implementation for ns cache
#    has a different interface)
#  * ns_cache_size 
#
# Install this file as /usr/local/ns/tcl/aolserver-openacs.tcl
#
# This script requires Tcl 8.5 or newer.
#
# Gustaf Neumann fecit June, 2009


if {1} {
    # In previous versions, this file was just loaded if not in the
    # regression test server. The exit from the test server calls the
    # global "exit" handler in XOTcl/NX (not the thread exit handler)
    # which used to show wrong error messages. This seems to be solved,
    # and we can use nsf in the regression test suite.

    # Requiring the XOTcl/NX and the serializer here is not necessary
    # for the ns-cache emulation, but since the tcl files are sourced in
    # alphabetical order, we make sure that we can use nx here (if
    # installed).

    #
    # What XOTcl should be loaded? If XOTcl 2 is chosen, but not
    # installed, it falls back and tries to load XOTcl 1.
    #

    set xotcl 2 ;# 1 or 2

    if {$xotcl == 2} {
        if {[catch {
            package require nsf
            ns_ictl trace delete {if {[info commands ::nsf::finalize] ne ""} {::nsf::finalize}}
            package require XOTcl 2
            package require nx::serializer
            namespace import -force ::xotcl::*
            ns_log notice "XOTcl [package require XOTcl 2] loaded"
        }]} {
            # We could not load XOTcl 2; fall back and try to load XOTcl 1
            set xotcl 1
        }
    }
    
    if {$xotcl == 1} {
        catch {
            package require XOTcl 1
            package require -exact xotcl::serializer 1.0
            namespace import -force ::xotcl::*
            ns_log notice "XOTcl [package require XOTcl 1] loaded"
        }
    }
}

#
# ns_share is used in OpenACS for backward compatibility with ACS 2.*
# No active code uses code any more.
#
# See for details: ./acs-tcl/tcl/aolserver-3-procs.tcl
proc ns_share args {
    ns_log warning "Warning: 'ns_share $args' is not supported by NaviServer. \n\
    Most likely this is not used by your application; if so, it should be replaced\n\
    by nsv."
}

if {[info commands ::nx::Object] ne "" && [::nx::Object info lookup method object] ne ""} {
    ns_log notice "Using ns_cache based on NX [package require nx]"

    if {[info commands ::ad_log] eq ""} {
        #
        # Provide a minimal variant of OpenACS's value added version
        # of ns_log (showing e.g. call stack, etc.)
        #
        proc ::ad_log {level message} {ns_log $level $message}
    }
    
    #
    # Minimal ns_cache implementation based on NX
    #
    ::nx::Object create ::ns_cache {
        :public object method eval {cache_name key script} {
            set rc [catch {uplevel [list ns_cache_eval $cache_name $key $script]} result]
            return -code $rc $result
        }

        :public object alias flush ::ns_cache_flush

        :public object method create {cache_name {-size 1024000} {-timeout}} {
            # expire in NS means timeout in AOLserver 
            if {[info exists timeout]} {
                set create_cmd "ns_cache_create -expires $timeout $cache_name $size"  
            } else {
                set create_cmd "ns_cache_create $cache_name $size"
            }
            return  [{*}$create_cmd]
        }

        :public object method names {cache_name args} {
            set ts0 [clock clicks -milliseconds]
            set r [ns_cache_keys $cache_name {*}$args]
            set span [expr {[clock clicks -milliseconds] - $ts0}]
            if {$span > 200} {
                ad_log notice "!!!! long ns_cache_names $span ms, ns_cache names $cache_name $args"
            }
            return $r
        }

        :public object method get {cache_name key var_name:optional} {
            if {[info exists var_name]} {
                return [uplevel [list ns_cache_get $cache_name $key $var_name]]
            } else {
                return [ns_cache_get $cache_name $key]
            }
        }

        :public object method set {cache_name key value} {
            uplevel ns_cache_eval -force -- $cache_name [list $key] [list set _ $value]
        }

        :object method unknown {subcmd cache_name args} {
            ns_log notice "ns_cache unknown, subcmd=$subcmd, args=$args"
            set ts0 [clock clicks -milliseconds]
            #ns_log notice "ns_cache $subcmd $cache_name"
            set rc [catch {uplevel ns_cache_$subcmd $cache_name $args} result]
            set span [expr {[clock clicks -milliseconds] - $ts0}]
            if {$span > 200} {
                ad_log notice "!!!! long ns_cache $subcmd $span ms, ns_cache $subcmd $cache_name $args"
            }
            #if {$rc != 0} {ns_log notice "EVAL returned code=$rc result='$result'"}
            return -code $rc $result
        }

    }
    
} else {
    ns_log notice "Using ns_cache implemented as a Tcl proc"
    #
    # Minimal ns_cache implementation implemented as a Tcl proc
    #
    proc ns_cache {cmd cache_name args} {
        switch $cmd {
            create {
                array set args_array $args
                if {[info exists args_array(-size)]} {
                    set size $args_array(-size)
                    unset args_array(-size)
                } else {
                    # no -size given, using AOLServer's default value
                    set size [expr {1024 * 1000}]
                }
                # expire in NS means timeout in AOLserver 
                if {[info exists args_array(-timeout)]} {
                    set args_array(-expires) $args_array(-timeout)
                    unset args_array(-timeout)
                }
                if {[llength [array get args_array]]} {
                    set create_cmd "ns_cache_$cmd [array get args_array] $cache_name $size"  
                } else {
                    set create_cmd "ns_cache_$cmd $cache_name $size"
                }
                set r [{*}$create_cmd]
                return $r
            }
            names {
                set ts0 [clock clicks -milliseconds]
                set r [ns_cache_keys $cache_name {*}$args]
                set span [expr {[clock clicks -milliseconds] - $ts0}]
                if {$span > 200} {
                    ns_log notice "!!!! long ns_cache $cmd $span ms, ns_cache $cmd $cache_name $args"
                }
                return $r
            }
            get {
                set key [lindex $args 0]
                if {[llength $args] > 1} {
                    set var_name [lindex $args 1]
                    # Check if we have an entry. This assumes that only valid (not
                    # expired) entries are returned, and this state will be true
                    # for the subsequence _eval as well.
                    if {[ns_cache keys $cache_name $key] ne ""} {
                        # The next pattern assumes, that the script == key, as in
                        # util_memoize
                        set r [ns_cache_eval $cache_name $key $key]
                        uplevel set $var_name [list $r]
                        return 1
                    } else {
                        return 0
                    }
                } else {
                    set r [ns_cache_eval $cache_name $key $key]
                    return $r
                }
            }
            set {
                # assuming: ns_cache set CACHE_NAME KEY VALUE
                set key [lindex $args 0]
                set value [lindex $args 1]
                uplevel ns_cache_eval -force -- $cache_name [list $key] [list set _ $value]
            }
            default {
                set ts0 [clock clicks -milliseconds]
                #ns_log notice "ns_cache $cmd $cache_name"
                set rc [catch {uplevel ns_cache_$cmd $cache_name $args} result]
                set span [expr {[clock clicks -milliseconds] - $ts0}]
                if {$span > 200} {
                    ns_log notice "!!!! long ns_cache $cmd $span ms, ns_cache $cmd $cache_name $args"
                }
                #if {$rc != 0} {ns_log notice "EVAL returned code=$rc result='$result'"}
                return -code $rc $result
            }
        }
    } 
}

# Managing ns_cache_size as in AOLServer 
# vguerra@wu.ac.at

proc ns_cache_size { cache_name } {
    array set stats [ns_cache_stats $cache_name]
    return [list $stats(maxsize) $stats(size)]
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
