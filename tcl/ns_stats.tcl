#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#

namespace eval ::ns {}

if {[info commands ::nx::Class] eq ""} {
    ns_log warning "NSF is not installed. The commands ::ns_stats is not available"
    return
}
if {$::tcl_version < 8.6} {
    # We need at least the "zlib" command.
    ns_log warning "The command ::ns_stats requires Tcl 8.6 or newer"
    return
}


# create namespace and global nsv
namespace eval ::ns::stats {
    nsv_array create -scope process ns_stats
    foreach key {control pair history recent recentDroppedThrough} {
        nsv_set -default ns_stats $key {}
    }
}

nx::Class create ::ns::stats::Collector {
    #
    #   Collect structured NaviServer topology and runtime diagnostics.
    #   describe returns process information, virtual servers, connection
    #   pools and driver configuration. snapshot returns current gauges,
    #   cumulative counters and thread CPU times.
    #
    #   Collection is synchronous and does not start sampling, advance
    #   interval baselines or persist data. An optional server argument
    #   restricts virtual-server data; process-wide metrics retain their
    #   process scope. Availability fields identify optional information
    #   that could not be collected.
    #
    #   Provides the collection interface inherited by ns::stats::Sampler.

    :method systemText {path} {
        set f [open $path r]
        try {return [string trim [read $f 1048576]]} finally {close $f}
    }
    :method cpuListCount {text} {
        set ranges {}
        foreach item [split $text ,] {
            if {![regexp {^([0-9]+)(?:-([0-9]+))?$} [string trim $item] -> first last]} {error "Invalid CPU list"}
            if {$last eq ""} {set last $first}
            if {$last<$first} {error "Invalid CPU range"}
            lappend ranges [list $first $last]
        }
        set count 0; set end -1
        foreach range [lsort -integer -index 0 $ranges] {
            lassign $range first last
            if {$last>$end} {incr count [expr {$last-max($first,$end+1)+1}]; set end $last}
        }
        return $count
    }
    :method mountPath {value} {
        return [string map [list {\040} " " {\011} "\t" {\134} "\\"] $value]
    }
    :public method cpuCapacity {} {
        set result {hostLogicalCpus {} affinityCpus {} cpusetCpus {} quotaCores {} effectiveCores {} quotaStatus unknown constraintsComplete false throttlingAvailable false throttlingScope cgroup-hierarchy throttling {} limitations {Visible CPU ceilings are not reserved capacity. Hidden ancestor limits, host contention and CPU shares can reduce availability. Throttling counters are cumulative for each visible cgroup, including other processes; ancestor counters can include sibling groups.}}
        set reasons {}; set ceilings {}
        if {![catch {:cpuListCount [:systemText /sys/devices/system/cpu/online]} count] && $count>0} {
            dict set result hostLogicalCpus $count; lappend ceilings $count
        }
        if {![catch {:systemText /proc/self/status} status]
            && [regexp -line {^Cpus_allowed_list:\s*([^\n]+)} $status -> list]
            && ![catch {:cpuListCount [string trim $list]} count] && $count>0} {
            dict set result affinityCpus $count; lappend ceilings $count
        } else {lappend reasons affinity-unavailable}
        set memberships {}
        if {![catch {:systemText /proc/self/cgroup} text]} {
            foreach line [split $text \n] {
                if {![regexp {^[0-9]+:([^:]*):(.*)$} $line -> controllers path]} continue
                if {$controllers eq ""} {dict set memberships unified $path}
                foreach controller [split $controllers ,] {dict set memberships $controller $path}
            }
        }
        set quotaSeen false; set quotaMissing false; set hierarchySeen false
        set groups {}; set visited {}
        if {![catch {:systemText /proc/self/mountinfo} mounts]} {
            foreach line [split $mounts \n] {
                set parts [split $line " "]
                set separator [lsearch -exact $parts -]
                if {$separator<6} continue
                set type [lindex $parts [expr {$separator+1}]]
                if {$type ni {cgroup cgroup2}} continue
                set root [:mountPath [lindex $parts 3]]
                set mount [:mountPath [lindex $parts 4]]
                set controllers [split [lindex $parts [expr {$separator+3}]] ,]
                set key {}; set cpu false; set cpuset false
                if {$type eq "cgroup2" && [dict exists $memberships unified]} {
                    set key unified; set cpu true; set cpuset true
                } else {
                    foreach controller {cpu cpuset} {
                        if {$controller in $controllers && [dict exists $memberships $controller]} {
                            set key $controller
                            if {$controller eq "cpu"} {set cpu true} else {set cpuset true}
                        }
                    }
                }
                if {$key eq ""} continue
                set member [dict get $memberships $key]
                # Do not traverse outside a visible mount, including namespace
                # membership paths containing parent components.
                if {".." in [file split $member]} {lappend reasons cgroup-path-unresolvable; continue}
                if {$root eq "/"} {set relative [string trimleft $member /]
                } elseif {$member eq "/" || $member eq $root} {set relative {}
                } elseif {[string first "$root/" $member]==0} {set relative [string range $member [expr {[string length $root]+1}] end]
                } else {continue}
                set directory [file join $mount $relative]
                set hierarchySeen true
                while {1} {
                    if {[lsearch -exact $visited [list $directory $cpu $cpuset]]<0} {
                        lappend visited [list $directory $cpu $cpuset]
                        if {$cpu} {
                            if {$type eq "cgroup2"} {
                                set readable [expr {![catch {:systemText $directory/cpu.max} maximum]}]
                                if {$readable && [regexp {^(max|[0-9]+) ([1-9][0-9]*)$} $maximum -> quota period]} {
                                    set quotaSeen true
                                    if {$quota ne "max" && $quota>0} {lappend ceilings [expr {double($quota)/$period}]; set q [expr {double($quota)/$period}]
                                        if {[dict get $result quotaCores] eq "" || $q<[dict get $result quotaCores]} {dict set result quotaCores $q}}
                                } elseif {$directory ne $mount} {set quotaMissing true}
                            } else {
                                if {![catch {:systemText $directory/cpu.cfs_quota_us} quota]
                                    && ![catch {:systemText $directory/cpu.cfs_period_us} period]
                                    && [string is entier -strict $quota] && [string is entier -strict $period] && $period>0} {
                                    set quotaSeen true
                                    if {$quota>0} {set q [expr {double($quota)/$period}]; lappend ceilings $q
                                        if {[dict get $result quotaCores] eq "" || $q<[dict get $result quotaCores]} {dict set result quotaCores $q}}
                                } else {set quotaMissing true}
                            }
                            if {![catch {:systemText $directory/cpu.stat} counters]} {
                                set row [dict create path $directory]
                                foreach counter [split $counters \n] {
                                    lassign $counter name value
                                    if {![string is entier -strict $value] || $value<0} continue
                                    switch -- $name {
                                        nr_periods {dict set row periods $value}
                                        nr_throttled {dict set row throttledPeriods $value}
                                        throttled_usec {dict set row throttledSeconds [expr {$value/1000000.0}]}
                                        throttled_time {dict set row throttledSeconds [expr {$value/1000000000.0}]}
                                    }
                                }
                                if {[dict size $row]>1} {dict set groups $directory $row}
                            }
                        }
                        if {$cpuset} {
                            set file [expr {$type eq "cgroup2" ? "cpuset.cpus.effective" : "cpuset.cpus"}]
                            if {![catch {:systemText $directory/$file} list] && $list ne "" && ![catch {:cpuListCount $list} count] && $count>0} {
                                lappend ceilings $count
                                if {[dict get $result cpusetCpus] eq "" || $count<[dict get $result cpusetCpus]} {dict set result cpusetCpus $count}
                            }
                        }
                    }
                    if {$directory eq $mount} break
                    set directory [file dirname $directory]
                }
            }
        }
        if {!$hierarchySeen} {lappend reasons cgroup-hierarchy-unavailable}
        if {!$quotaSeen || $quotaMissing} {lappend reasons quota-unavailable-or-partial}
        dict set result quotaStatus [expr {[dict get $result quotaCores] ne "" ? "limited" : ($quotaSeen && !$quotaMissing ? "unlimited" : "unknown")}]
        if {[llength $ceilings]} {dict set result effectiveCores [lindex [lsort -real $ceilings] 0]}
        dict set result constraintsComplete [expr {$reasons eq ""}]
        dict set result incompleteReasons [lsort -unique $reasons]
        dict set result throttling $groups
        dict set result throttlingAvailable [expr {[dict size $groups]>0}]
        return $result
    }

    :method selectServer {record server {validate true}} {
        # Restrict virtual-server data; historical records may refer to removed servers.
        if {$server ne ""} {
            if {$validate && $server ni [ns_info servers]} {
                error "unknown server: $server"
            }
            set selected {}
            if {[dict exists $record servers $server]} {
                dict set selected $server [dict get $record servers $server]
            }
            dict set record servers $selected
        }
        return $record
    }

    :public method describe {{-server {}}} {
        #
        #   Return a dictionary describing the NaviServer process, virtual
        #   servers, connection pools and network drivers. An optional server
        #   argument restricts virtual-server data; process-wide information
        #   retains its process scope. Availability fields identify optional
        #   information that could not be collected.
        #
        #   Does not start sampling or modify the sampling baseline.
        #
        set result [dict create schemaVersion 1 scope process \
                        process [dict create \
                                     pid [ns_info pid] \
                                     bootTime [ns_info boottime] \
                                     version [ns_info patchlevel] \
                                     host [ns_info hostname] \
                                     uptimeSeconds [ns_info uptime]] \
                        servers {} drivers {} availability {}]

        if {![catch {ns_info buildinfo} build]} {
            dict set result process build $build
        }
        dict set result process platform [dict create \
                                              os $::tcl_platform(os) \
                                              osVersion $::tcl_platform(osVersion) \
                                              machine $::tcl_platform(machine)]
        dict set result process cpuCapacity [:cpuCapacity]
        foreach s [ns_info servers] {
            set pools {}
            foreach p [ns_server -server $s pools] {
                set name [expr {$p eq "" ? "default" : $p}]
                set threads [ns_server -server $s -pool $p threads]
                set limits {}
                foreach k {min max maxconnections} {
                    if {[dict exists $threads $k]} {
                        dict set limits $k [dict get $threads $k]
                    }
                }
                dict set pools $name $limits
            }
            set entry [dict create \
                           pools $pools \
                           databasePools [ns_config ns/server/$s/db pools {}] bindings {} availability {}]
            if {![catch {ns_server -server $s modules} modules]} {
                dict set entry modules $modules
                dict set entry availability modules true
            } else {
                dict set entry availability modules false
            }
            if {![catch {ns_driver info} infos]} {
                foreach driver $infos {
                    set module [dict get $driver module]
                    set mappings {}
                    set allMappings true
                    if {[catch {ns_config -all ns/module/$module/servers $s {}} mappings]} {
                        set mappings [ns_config ns/module/$module/servers $s {}]
                        set allMappings false
                    }
                    if {$mappings ne "" || [dict get $driver server] eq $s} {
                        dict set entry bindings $module [dict create \
                                                             address [dict get $driver address] \
                                                             port [dict get $driver port] \
                                                             protocol [dict get $driver protocol] \
                                                             hostMappings $mappings \
                                                             hostMappingsComplete $allMappings]
                    }
                }
            }
            dict set result servers $s $entry
        }

        if {![catch {ns_driver info} info]} {
            set drivers {}
            foreach driver $info {
                set entry {}
                foreach k {
                    module type server location address port defaultport protocol
                    sendwait recvwait recvbufsize sendbufsize libraryversion
                } {
                    if {[dict exists $driver $k]} {
                        dict set entry $k [dict get $driver $k]
                    }
                }
                lappend drivers $entry
            }
            dict set result drivers $drivers
            dict set result availability drivers true
        } else {
            dict set result availability drivers false
        }
        return [:selectServer $result $server]
    }

    :public method snapshot {{-server {}}} {
        #   Collect current gauges, cumulative counters, allocator statistics
        #   and thread CPU times into a timestamped dictionary. The timestamp
        #   is in epoch microseconds; collectionSeconds records collection
        #   duration. The observations are collected sequentially and do not
        #   constitute an atomic snapshot of the entire process.
        #
        #   An optional server argument restricts virtual-server data.
        #   Does not publish a sample or advance the sampling baseline.

        set start [clock microseconds]
        set result [dict create \
                        schemaVersion 1 \
                        scope process \
                        process [dict create pid [pid] bootTime [ns_info boottime]] \
                        drivers {} \
                        servers {} \
                        cpu {} \
                        availability {} \
                        units {time seconds cpuCounter microseconds cpuPercent percentOfOneCore}]
        dict set result process cpuCapacity [:cpuCapacity]
        if {![catch {ns_driver stats} stats]} {
            foreach entry $stats {
                set counters {}
                foreach key {received partial spooled errors} {
                    dict set counters $key [dict get $entry $key]
                }
                set socketStates [dict create \
                                      reading [dict get $entry reading] \
                                      waitingForConnection [dict get $entry waiting] \
                                      closing [dict get $entry closing]]
                dict set result drivers [dict get $entry thread] \
                    [dict create thread [dict get $entry thread] \
                         module [dict get $entry module] \
                         counters $counters socketStates $socketStates]
            }
            dict set result availability drivers true
        } else {
            dict set result availability drivers false
        }
        foreach s [ns_info servers] {
            foreach p [ns_server -server $s pools] {
                set name [expr {$p eq "" ? "default" : $p}]
                dict set result servers $s pools $name \
                    [dict create \
                         counters [ns_server -server $s -pool $p stats] \
                         gauges [dict create \
                                     threads [ns_server -server $s -pool $p threads] \
                                     waitingRequests [ns_server -server $s -pool $p waiting] \
                                     activeRequests [llength [ns_server -server $s -pool $p active]]]]
            }
        }

        set cpu {}
        if {![catch {ns_info threadcputimes} times]} {
            foreach t [ns_info threads] {
                set tid [lindex $t 7]
                if {$tid eq "" || ![dict exists $times $tid]} {
                    continue
                }
                set identity [list $tid [lindex $t 4]]
                dict set cpu $identity [dict merge [dict get $times $tid] [dict create name [lindex $t 0]]]
            }
        }
        set memory {}

        if {![catch {ns_info meminfo} info]} {
            foreach k {
                current_allocated_bytes heap_size total_physical_bytes slack_bytes
                pageheap_free_bytes pageheap_unmapped_bytes pageheap_committed_bytes
                central_cache_free_bytes transfer_cache_free_bytes thread_cache_free_bytes
                current_total_thread_cache_bytes
            } {
                if {[dict exists $info $k]} {
                    dict set memory $k [dict get $info $k]
                }
            }
        }
        dict set result process allocatorBytes $memory
        dict set result availability allocatorMemory [expr {$memory ne ""}]
        dict set result cpu $cpu
        dict set result availability cpu [expr {$cpu ne ""}]
        if {![catch {ns_db stats} dbstats]} {
            dict set result databasePools $dbstats
            dict set result availability databasePools true
        } else {
            dict set result availability databasePools false
        }
        dict set result timestamp [clock microseconds]
        dict set result collectionSeconds [expr {([dict get $result timestamp]-$start)/1000000.0}]
        return [:selectServer $result $server]
    }
}

nx::Class create ::ns::stats::Sampler -superclass ::ns::stats::Collector {
    #
    #   Extend Collector with process-wide periodic sampling and interval
    #   calculations. Keep configuration, collection status and the latest
    #   sample in the process-scoped ns_stats nsv array. Coordinate updates
    #   through nsv_array eval so callers in different virtual servers use
    #   one sampler and one baseline.
    #
    #   Sampling starts explicitly. Each callback collects a snapshot,
    #   derives counter deltas, rates, average request phase times and CPU
    #   percentages, then publishes the completed sample. The first sample
    #   establishes a baseline and has no usable interval measurements.
    #
    #   Generation checks prevent an outstanding collection from publishing
    #   after the sampler has been stopped or restarted. Collection failures
    #   preserve the previous sample and update the shared error status.
    #
    #   Reading status or sample does not advance the baseline. Optional
    #   server selection filters virtual-server data without attributing
    #   process-wide measurements to an individual server.
    #
    #   Persistence is supplied separately by the SQLiteHistory mixin.

    :method locked {script} {
        # Evaluate shared-state updates in the calling method's frame.
        uplevel 1 [list nsv_array eval ns_stats $script]
    }

    :method delta {now:double before:double} {
        # A decreased cumulative counter has no usable interval delta.
        if {$now < $before} {
            return {}
        }
        expr {$now - $before}
    }

    :method init {} {
        :locked {
            if {[nsv_get ns_stats control] eq ""} {
                nsv_set ns_stats control [dict create \
                                              running false interval 10 scheduler {} generation 0 busy false \
                                              instance "[pid]:[ns_info boottime]:[clock microseconds]" \
                                              errors 0 lastError {}]
            }
        }
    }
    :public method "sampling status" {} {
        #
        #   Return the process-wide sampler configuration and status, including
        #   running state, scheduler identifier, interval, process instance,
        #   collection errors and readiness of the latest interval sample.
        #
        #   latestAt is in epoch seconds; ageSeconds is the elapsed time since
        #   the latest publication. Both are empty before the first sample.
        #   Does not collect new observations or modify sampler state.

        set result [nsv_get ns_stats control]
        dict unset result busy
        dict unset result generation
        set sample [nsv_get ns_stats pair]
        dict set result ready [expr {$sample ne "" && [dict get $sample sampling ready]}]
        dict set result latestAt {}
        dict set result ageSeconds {}
        if {$sample ne ""} {
            set stamp [dict get $sample timestamp]
            dict set result latestAt [expr {$stamp/1000000.0}]
            dict set result ageSeconds [expr {max(0,([clock microseconds]-$stamp)/1000000.0)}]
        }
        return $result
    }

    :public method "sampling start" {{-interval 10}} {
        #   Start the process-wide sampling callback at the requested interval
        #   in seconds (default 10, range 1..3600). Time units and fractional
        #   seconds are normalized with ns_baseunit. Repeated calls with the
        #   same running interval reuse the scheduler; a different interval
        #   requires stopping the sampler first.
        #
        #   Clears the previous sample and baseline when starting a stopped
        #   sampler. Returns the resulting status dictionary. Persistence
        #   must be enabled separately.

        set interval [ns_baseunit -time $interval]
        if {$interval < 1 || $interval > 3600} {
            error "interval must be 1..3600 seconds"
        }
        :locked {
            set state [nsv_get ns_stats control]
            if {[dict get $state running]} {
                if {[dict get $state interval]!=$interval} {
                    error "sampler already running with a different interval; stop it before reconfiguring"
                }
            } else {
                dict incr state generation
                dict set state running true
                dict set state interval $interval
                dict set state busy false
                dict set state scheduler [ns_schedule_proc -thread $interval [self] sampling tick]
                nsv_set ns_stats pair {}
                nsv_set ns_stats control $state
            }
        }
        return [:sampling status]
    }

    :public method "sampling stop" {} {
        #
        #   Unschedule collection and invalidate any collection already in
        #   progress so it cannot publish into the stopped sampler.
        #
        #   Retains the latest published sample for subsequent reads.
        #   Returns the resulting status dictionary. Does not disable the
        #   separately configured history persistence callback.
        #
        :locked {
            set state [nsv_get ns_stats control]
            if {[dict get $state running]} {
                ns_unschedule_proc [dict get $state scheduler]
            }
            dict set state running false
            dict incr state generation
            dict set state busy false
            dict set state scheduler {}
            nsv_set ns_stats control $state
        }
        return [:sampling status]
    }

    :public method sample {{-server {}} {-window 0}} {
        #
        #   Return the latest published sample, updating its reported age and
        #   running state. Before any publication, return a dictionary with
        #   sampling readiness false and no interval observations.
        #
        #   An optional server argument restricts virtual-server data; CPU
        #   measurements remain process-wide. Without -window, does not collect
        #   observations or advance the baseline. With -window (0..900 seconds, time units
        #   accepted), collect a fresh snapshot and compare it with the nearest retained
        #   endpoint to that age (within half a sampling interval). Missing
        #   history, gaps or sampler restarts leave readiness false. The
        #   measured elapsedSeconds reports the actual selected period.
        #   The internal raw baseline is omitted
        #   from the returned dictionary.
        #
        if {$server ne "" && $server ni [ns_info servers]} {
            error "unknown server: $server"
        }
        set window [ns_baseunit -time $window]
        if {$window < 0 || $window > 900} {
            error "display window must be 0..900 seconds"
        }
        :locked {
            set result [nsv_get ns_stats pair]
            set recent [expr {$window > 0 ? [nsv_get ns_stats recent] : {}}]
        }
        if {$result eq ""} {
            return [dict create schemaVersion 1 \
                        sampling {ready false from {} to {} elapsedSeconds {} ageSeconds {} reset false} \
                        servers {} availability {}]
        }
        if {$window > 0} {
            set current [:snapshot]
            set target [expr {[dict get $current timestamp]-$window*1000000}]
            set baseline {}
            set distance Inf
            foreach record $recent {
                if {[dict get $record timestamp] >= [dict get $current timestamp]
                    || [dict get $record instance] ne [dict get $result instance]} {
                    continue
                }
                set difference [expr {abs([dict get $record timestamp]-$target)}]
                if {$difference < $distance} {
                    set baseline $record
                    set distance $difference
                }
            }
            set cadence [dict get $result sampling intervalSeconds]
            set reason {}
            if {$baseline eq "" || $distance > 0.5*$cadence*1000000} {
                set baseline {}
                set reason insufficient-history
            } else {
                set previous $baseline
                foreach record $recent {
                    if {[dict get $record timestamp] <= [dict get $baseline timestamp]
                        || [dict get $record timestamp] > [dict get $current timestamp]} {
                        continue
                    }
                    if {[dict get $record instance] ne [dict get $result instance]
                        || ![dict get $record sampling ready]
                        || [dict get $record timestamp]-[dict get $previous timestamp]
                           > 1.5*max($cadence,[dict get $previous sampling intervalSeconds])*1000000} {
                        set reason sampling-gap
                        set baseline {}
                        break
                    }
                    set previous $record
                }
            }
            if {$baseline ne "" && [dict get $current timestamp]-[dict get $previous timestamp]
                > 1.5*$cadence*1000000} {
                set baseline {}
                set reason sampling-gap
            }
            # Older retained records may predate CPU baseline retention.
            if {$baseline ne "" && ![dict exists $baseline cpu]} {
                dict set baseline cpu {}
            }
            set result [:calculate $current $baseline $cadence [dict get $result instance]]
            dict set result sampling requestedSeconds $window
            dict set result sampling incompleteReason $reason
        }
        dict set result sampling running [dict get [nsv_get ns_stats control] running]
        dict set result sampling ageSeconds [expr {max(0,([clock microseconds]-[dict get $result timestamp])/1000000.0)}]
        if {[dict exists $result raw]} {dict unset result raw}
        # CPU is process-wide; filtering server pools never attributes CPU to a vhost.
        return [:selectServer $result $server]
    }

    :public method calculate {current previous interval instance} {
        #
        #   Derive interval diagnostics from current and previous snapshots.
        #   Return the current observations augmented with counter deltas,
        #   rates, average request phase times and thread CPU percentages.
        #   Rates use measured elapsed time; interval records the configured
        #   sampling cadence, and instance identifies the sampler lifetime.
        #
        #   Interval results require a previous snapshot from the same process
        #   lifetime and positive elapsed time. Missing baselines or decreased
        #   counters yield empty derived values. CPU comparisons match thread
        #   identifiers and creation times; percentages use one core as 100%.
        #
        #   Does not collect observations or modify shared state.
        #
        set stamp [dict get $current timestamp]
        set elapsed {}
        set ready false
        set reset false
        if {$previous ne ""
            && [dict get $previous process pid] eq [dict get $current process pid]
            && [dict get $previous process bootTime] eq [dict get $current process bootTime]
        } {
            set elapsed [expr {($stamp-[dict get $previous timestamp])/1000000.0}]
            if {$elapsed > 0} {
                set ready true
            } else {
                set reset true
            }
        }
        set result $current
        dict set result instance $instance
        dict set result sampling [dict create \
                                      ready $ready \
                                      from {} \
                                      to [expr {$stamp/1000000.0}] \
                                      elapsedSeconds $elapsed \
                                      ageSeconds 0 \
                                      reset $reset \
                                      intervalSeconds $interval]
        if {$ready} {
            dict set result sampling from [expr {[dict get $previous timestamp]/1000000.0}]
        }

        dict for {s server} [dict get $current servers] {
            dict for {p entry} [dict get $server pools] {
                set deltas {}
                set rates {}
                set invalid {}
                dict for {k v} [dict get $entry counters] {
                    if {$k ni {requests queued spools spooled dropped queuetime filtertime runtime tracetime}} {
                        continue
                    }
                    set d {}
                    if {$ready && [dict exists $previous servers $s pools $p counters $k]} {
                        set d [:delta $v [dict get $previous servers $s pools $p counters $k]]
                        if {$d eq ""} {
                            lappend invalid $k
                        }
                    }
                    dict set deltas $k $d
                    dict set rates $k [expr {$d eq "" ? "" : $d/$elapsed}]
                }
                dict set result servers $s pools $p deltas $deltas
                dict set result servers $s pools $p rates $rates
                dict set result servers $s pools $p resetCounters $invalid
                set timings {}
                set n [expr {[dict exists $deltas requests] ? [dict get $deltas requests] : ""}]

                foreach k {queuetime filtertime runtime tracetime} {
                    set d [expr {[dict exists $deltas $k] ? [dict get $deltas $k] : ""}]
                    dict set timings $k [expr {$n eq "" || $n == 0 || $d eq "" ? "" : $d/$n}]
                }
                dict set result servers $s pools $p timingAverageSeconds $timings
            }
        }

        dict for {thread entry} [dict get $current drivers] {
            set deltas {}
            set rates {}
            set invalid {}
            foreach k {received partial spooled errors} {
                set d {}
                if {$ready && [dict exists $entry counters $k] && [dict exists $previous drivers $thread counters $k]} {
                    set d [:delta [dict get $entry counters $k] [dict get $previous drivers $thread counters $k]]
                    if {$d eq ""} {
                        lappend invalid $k
                    }
                }
                dict set deltas $k $d
                dict set rates $k [expr {$d eq "" ? "" : $d/$elapsed}]
            }
            dict set result drivers $thread deltas $deltas
            dict set result drivers $thread rates $rates
            dict set result drivers $thread resetCounters $invalid
        }

        set percentages {}
        set totals {}
        set matched 0

        dict for {id entry} [dict get $current cpu] {
            set percent {}
            if {$ready && [dict exists $previous cpu $id]} {
                set old [dict get $previous cpu $id]
                set user [:delta [dict get $entry user] [dict get $old user]]
                set system [:delta [dict get $entry system] [dict get $old system]]
                if {$user ne "" && $system ne ""} {
                    set percent [expr {100.0*($user+$system)/($elapsed*1000000)}]
                    incr matched
                }
            }
            set name [string trim [dict get $entry name] -]
            set group other
            foreach {pattern kind} {driver:* driver conn:* connection writer:* writer spooler:* spooler} {
                if {[string match $pattern $name]} {
                    set group $kind
                    break
                }
            }
            dict set percentages $id [dict create name $name group $group percent $percent]
            if {$percent ne ""} {
                set total [expr {[dict exists $totals $group] ? [dict get $totals $group] : 0.0}]
                dict set totals $group [expr {$total+$percent}]
            }
        }
        if {[dict exists $current process cpuCapacity throttling]} {
            dict for {path row} [dict get $current process cpuCapacity throttling] {
                foreach field {periods throttledPeriods throttledSeconds} {
                    set delta {}
                    if {$ready && [dict exists $row $field] && [dict exists $previous process cpuCapacity throttling $path $field]} {
                        set old [dict get $previous process cpuCapacity throttling $path $field]
                        if {[dict get $row $field]>=$old} {set delta [expr {[dict get $row $field]-$old}]}
                    }
                    dict set row delta[string toupper [string index $field 0]][string range $field 1 end] $delta
                }
                dict set row throttledPeriodPercent {}
                if {[dict get $row deltaPeriods] ne "" && [dict get $row deltaPeriods]>0 && [dict get $row deltaThrottledPeriods] ne ""} {
                    dict set row throttledPeriodPercent [expr {100.0*[dict get $row deltaThrottledPeriods]/[dict get $row deltaPeriods]}]
                }
                dict set result process cpuCapacity throttling $path $row
            }
        }
        dict set result cpuPercent [dict create \
                                        scope process \
                                        matchedThreads $matched \
                                        observedThreads [dict size [dict get $current cpu]] \
                                        groups $totals \
                                        threads $percentages]
        return $result
    }

    :method retainSample {sample} {
        # Called while holding the existing ns_stats coordination lock.
        # Keep bounded endpoint CPU counters for display-window calculations.
        # Raw baselines and derived per-thread maps are not retained.
        set record [dict remove $sample raw]
        if {[dict exists $record cpuPercent threads]} {
            dict unset record cpuPercent threads
        }
        set records [nsv_get ns_stats recent]
        lappend records $record
        set cutoff [expr {[dict get $record timestamp]-900000000}]
        set dropped [nsv_get ns_stats recentDroppedThrough]
        while {[llength $records]>180
               || ([llength $records]>1 && [dict get [lindex $records 0] timestamp]<$cutoff)} {
            set dropped [dict get [lindex $records 0] timestamp]
            set records [lrange $records 1 end]
        }
        nsv_set ns_stats recent $records
        nsv_set ns_stats recentDroppedThrough $dropped
    }

    :public method "sampling history" {
        -from:integer,required
        -to:integer,required
        {-server {}}
        {-limit:integer 180}
        {-cursor:integer 0}
    } {
        #   Read the bounded process-wide recent sample buffer without SQLite.
        #   Retains at most 180 compact samples and 15 minutes at publication.
        #   Returns interval metrics, coverage and timestamp-based paging.
        #   Raw/per-thread CPU counters are omitted. Reading does not collect,
        #   start sampling, advance baselines or write persistent history.
        #   The buffer survives sampler stop/start but not a process restart.
        #
        if {$from<0 || $to<=$from || $to-$from>86400 || $to>[clock seconds]
            || $limit<1 || $limit>180 || $cursor<0} {
            error "expected an epoch window up to 24 hours, limit 1..180 and nonnegative cursor"
        }
        if {[string length $server]>512 || [string first \x00 $server]>=0} {
            error "invalid server name"
        }
        :locked {
            set retained [nsv_get ns_stats recent]
            set dropped [nsv_get ns_stats recentDroppedThrough]
        }
        set records {}
        set gaps {}
        set restarts {}
        set resets 0
        set previous {}
        set first {}
        set last {}
        set more false
        foreach record $retained {
            set stamp [dict get $record timestamp]
            if {$first eq ""} {
                set first $stamp
            }
            set last $stamp
            if {$stamp<$from*1000000 || $stamp<=$cursor} {
                set previous $record
                continue
            }
            if {$stamp>=$to*1000000} {
                # A gap crossing the upper bound must not certify coverage.
                if {$previous ne ""} {
                    set delta [expr {($stamp-[dict get $previous timestamp])/1000000.0}]
                    if {$delta>1.5*max([dict get $previous sampling intervalSeconds],
                                       [dict get $record sampling intervalSeconds])} {
                        lappend gaps [dict create from [dict get $previous sampling to] to [dict get $record sampling to]]
                    }
                }
                break
            }
            if {[llength $records]>=$limit} {
                set more true
                break
            }
            if {![dict get $record sampling ready]} {
                incr resets
            }
            if {$previous ne ""} {
                if {[dict get $previous instance] ne [dict get $record instance]} {
                    lappend restarts [dict get $record sampling to]
                }
                set delta [expr {($stamp-[dict get $previous timestamp])/1000000.0}]
                if {$delta>1.5*max([dict get $previous sampling intervalSeconds],
                                   [dict get $record sampling intervalSeconds])} {
                    lappend gaps [dict create from [dict get $previous sampling to] to [dict get $record sampling to]]
                }
            }
            lappend records [:selectServer [dict remove $record cpu] $server false]
            set previous $record
        }
        # Retained bounds refer to the buffer, independently of the selected page.
        if {$retained ne ""} {
            set first [dict get [lindex $retained 0] timestamp]
            set last [dict get [lindex $retained end] timestamp]
        }
        set overlap [expr {$dropped ne "" && $dropped>=$from*1000000}]
        if {!$more && $cursor>0} {
            set window [:sampling history -from $from -to $to -server $server -limit 180]
            set coverage [dict get $window coverage]
            return [dict create schemaVersion 1 source memory from $from to $to records $records \
                        nextCursor {} more false retentionSeconds 900 maxSamples 180 coverage $coverage]
        }
        set complete [expr {$first ne "" && $first<=$from*1000000 && $last>=$to*1000000
                             && !$more && $cursor==0 && !$overlap && $gaps eq ""
                             && $restarts eq "" && $resets==0}]
        set next {}
        if {$more} {
            set next [dict get [lindex $records end] timestamp]
        }
        return [dict create schemaVersion 1 source memory from $from to $to records $records \
                    nextCursor $next more $more retentionSeconds 900 maxSamples 180 \
                    coverage [dict create complete $complete windowExamined [expr {!$more && $cursor==0}] \
                        retainedFrom [expr {$first eq "" ? 0 : $first/1000000.0}] \
                        retainedTo [expr {$last eq "" ? 0 : $last/1000000.0}] \
                        gaps $gaps restartBoundaries $restarts resetSamples $resets \
                        evictionOrFailureOverlap $overlap droppedThrough $dropped]]
    }

    :public method "sampling tick" {} {
        #
        #   Perform one scheduled collection and publish its interval sample.
        #   Skip collection when the sampler is stopped or another collection
        #   is in progress. Collect observations outside the coordination lock,
        #   then calculate and publish only if the sampler generation is still
        #   current.
        #
        #   Retain the raw snapshot as the next interval baseline. Collection
        #   failures retain the previous publication, update error status and
        #   emit a warning. Does not write SQLite history; persistence runs
        #   through a separate callback.
        #
        set generation {}
        :locked {
            set state [nsv_get ns_stats control]
            if {![dict get $state running] || [dict get $state busy]} {
                return
            }
            set generation [dict get $state generation]
            dict set state busy true
            nsv_set ns_stats control $state
        }
        if {$generation eq ""} {
            return
        }
        try {
            set current [:snapshot]
            :locked {
                set state [nsv_get ns_stats control]
                if {$generation!=[dict get $state generation]} {
                    return
                }
                set pair [nsv_get ns_stats pair]
                set previous [expr {$pair eq "" ? "" : [dict get $pair raw]}]
                set sample [:calculate $current $previous [dict get $state interval] [dict get $state instance]]
                :retainSample $sample
                dict set sample raw $current
                nsv_set ns_stats pair $sample
                dict set state busy false
                dict set state lastError {}
                nsv_set ns_stats control $state
            }
        } on error {message options} {
            :locked {
                set state [nsv_get ns_stats control]
                if {$generation == [dict get $state generation]} {
                    dict set state busy false
                    dict incr state errors
                    dict set state lastError $message
                    nsv_set ns_stats control $state
                }
            }
            ns_log warning "ns_stats: sampling failed: $message"
        }
    }
}

nx::Class create ::ns::stats::SQLiteHistory {
    #
    #   Provide optional persistence for Sampler as an object mixin. Store
    #   published samples and associated topology in a dedicated SQLite
    #   database accessed through a configured native nsdbsqlite pool.
    #   Database handles are acquired per operation and are never retained
    #   in objects belonging to the interpreter blueprint.
    #
    #   A separate scheduled callback persists new sample timestamps.
    #   Database operations run outside the live sampling coordination
    #   lock, so persistence failures do not prevent sample publication.
    #   Samples are compressed; unchanged topology is stored once and
    #   referenced by subsequent samples.
    #
    #   Bound retained history by age and database size. Record eviction
    #   and write-failure information for coverage reporting, and reuse
    #   freed database pages. Disabling persistence leaves stored history
    #   intact. Process instance identifiers distinguish samples collected
    #   before and after a restart.
    #
    #   History queries return stored observations, topology, paging
    #   information and coverage metadata, including gaps, resets, restart
    #   boundaries and evictions. They do not resample stored measurements.
    #   Queries execute in the configuring virtual server, which provides
    #   access to the database pool.
    #
    #   Configuration and persistence status are shared through the
    #   process-scoped ns_stats nsv array. Sampling and persistence must
    #   each be enabled explicitly.
    #
    :method pack {value} {
        # Keep the stored tcl-zlib-hex-v1 representation unchanged.
        return [binary encode hex [zlib compress [encoding convertto utf-8 $value] 1]]
    }

    :method unpack {value} {
        # Decode stored text without losing Unicode or embedded NULs.
        return [encoding convertfrom utf-8 [zlib decompress [binary decode hex $value]]]
    }

    :method quote {value} {
        # Payloads are hex-encoded by pack; other values use standard nsdb quoting.
        return [ns_dbquotevalue $value]
    }

    # Fixed SQL statements use quote for values. Fetch only non-null columns
    # (including aggregate coalesce), since older nsdbsqlite drivers cannot
    # return SQL NULL safely. Flush pending rows and release the row set.
    :method sql {db query} {
        if {[ns_db exec $db $query] ne "NS_ROWS"} {
            return {}
        }
        set row [ns_db bindrow $db]
        set rows {}
        try {
            while {[ns_db getrow $db $row]} {
                set result {}
                for {set i 0} {$i<[ns_set size $row]} {incr i} {
                    dict set result [ns_set key $row $i] [ns_set value $row $i]
                }
                lappend rows $result
            }
        } finally {
            ns_db flush $db
            ns_set free $row
        }
        return $rows
    }

    :method scalar {db query} {
        # Return the first column of the first row, or empty for no rows.
        return [lindex [dict values [lindex [:sql $db $query] 0]] 0]
    }

    :method historyConnect {config} {
        set db [ns_db gethandle -timeout 1 [dict get $config pool]]
        if {$db eq ""} {
            error "history pool checkout timed out"
        }
        try {
            if {[ns_db dbtype $db] ne "sqlite"} {
                error "history requires an nsdbsqlite pool"
            }
            set path [ns_db datasource $db]
            if {$path eq "" || $path eq ":memory:" || [string match file:* $path]} {
                error "history requires a persistent file datasource"
            }
            file attributes $path -permissions 0600
            # A dedicated database avoids altering another application's budget.
            if {[:scalar $db {
                SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT GLOB 'ns_stats_*' AND name NOT GLOB 'sqlite_*'}]
            } {
                error "history requires a dedicated database"
            }
            foreach query {
                {PRAGMA busy_timeout=1000}
                {PRAGMA journal_mode=DELETE}
                {PRAGMA synchronous=FULL}
                {PRAGMA temp_store=MEMORY}
            } {
                :sql $db $query
            }
            set page [:scalar $db {PRAGMA page_size}]
            set maximum [expr {int([dict get $config maxBytes]*0.45)/$page}]

            if {[:scalar $db {PRAGMA page_count}]>$maximum} {
                error "existing history database exceeds byte budget"
            }
            :sql $db "PRAGMA max_page_count=$maximum"
            return $db
        } on error {message options} {
            ns_db releasehandle $db
            return -options $options $message
        }
    }

    :method historySchema {db} {
        foreach query {
            {CREATE TABLE IF NOT EXISTS ns_stats_meta (key TEXT PRIMARY KEY,value TEXT NOT NULL)}
            {CREATE TABLE IF NOT EXISTS ns_stats_topology (id TEXT PRIMARY KEY,payload TEXT NOT NULL)}
            {CREATE TABLE IF NOT EXISTS ns_stats_samples (at INTEGER PRIMARY KEY,instance TEXT NOT NULL,topology TEXT NOT NULL,payload TEXT NOT NULL)}
            {CREATE TABLE IF NOT EXISTS ns_stats_events (id INTEGER PRIMARY KEY,at INTEGER NOT NULL,first INTEGER NOT NULL,last INTEGER NOT NULL,n INTEGER NOT NULL,reason TEXT NOT NULL)}
        } {
            :sql $db $query
        }
        set version [:scalar $db {SELECT value FROM ns_stats_meta WHERE key='schemaVersion'}]
        if {$version ne "" && $version ne "1"} {
            error "unsupported diagnostics history schema"
        }
        set source [:quote [list [ns_info hostname] [ns_info home]]]
        set stored [:scalar $db {SELECT value FROM ns_stats_meta WHERE key='source'}]
        if {$stored ne "" && [:quote $stored] ne $source} {
            error "history database belongs to another process installation"
        }
        :sql $db {INSERT OR IGNORE INTO ns_stats_meta VALUES('schemaVersion','1')}
        :sql $db {INSERT OR IGNORE INTO ns_stats_meta VALUES('encoding','tcl-zlib-hex-v1')}
        :sql $db "INSERT OR IGNORE INTO ns_stats_meta VALUES('source',$source)"
    }

    :public method "history configure" {
        -pool:required
        {-retention 86400}
        {-maxBytes 67108864}
    } {
        #
        #   Validate the database pool and retention limits, initialize the
        #   schema and start a separate persistence callback. Repeated
        #   configuration with identical settings is harmless; changing
        #   active settings requires disabling persistence first.
        #   Return the resulting history status. Does not start sampling.
        #   Normalize retention to fractional seconds and maxBytes to bytes
        #   with ns_baseunit before validating the bounds.
        #
        set retention [ns_baseunit -time $retention]
        set maxBytes [ns_baseunit -size $maxBytes]
        if {$retention < 60 || $retention > 604800 || $maxBytes < 4194304} {
            error "retention must be 60..604800 seconds and maxBytes at least 4 MiB"
        }
        if {[info commands ns_db] eq ""} {
            error "history requires nsdb/nsdbsqlite"
        }
        set config [dict create \
                        enabled true pool $pool retention $retention maxBytes $maxBytes \
                        owner [ns_info server]  busy false scheduler {} \
                        generation [clock microseconds] lastSaved {} \
                        failures 0 lastFailure {} lastLoggedAt 0 gaps {} \
                        mixins [:info object mixins] topology {} topologyAt 0]
        set old [nsv_get ns_stats history]
        if {$old ne "" && [dict get $old enabled]} {
            foreach k {pool retention maxBytes owner} {
                if {[dict get $old $k] ne [dict get $config $k]} {
                    error "history already configured differently; disable before reconfiguring"
                }
            }
            return [:history status]
        }
        set db [:historyConnect $config]
        try {
            :historySchema $db
        } finally {
            ns_db releasehandle $db
        }
        :locked {
            set old [nsv_get ns_stats history]
            if {$old ne "" && [dict get $old enabled]} {
                error "history was concurrently configured; retry"
            }
            dict set config scheduler [ns_schedule_proc -thread 1 [self] history persist]
            nsv_set ns_stats history $config
        }
        return [:history status]
    }

    :public method "history disable" {} {
        #
        #   Stop future persistence callbacks without deleting stored history
        #   or stopping sampling. An outstanding write may finish.
        #   Return the resulting history status.
        #
        :locked {
            set config [nsv_get ns_stats history]
            if {$config ne ""} {
                if {[dict get $config enabled] && [dict get $config scheduler] ne ""} {
                    ns_unschedule_proc [dict get $config scheduler]
                }
                dict set config enabled false
                dict set config scheduler {}
                nsv_set ns_stats history $config
            }
        }
        return [:history status]
    }

    :public method "history status" {} {
        #
        #   Return shared history configuration and persistence status without
        #   opening the database. Internal coordination and cached topology
        #   fields are omitted.
        #
        set config [nsv_get ns_stats history]
        if {$config eq ""} {
            return {enabled false}
        }
        dict unset config busy
        dict unset config mixins
        dict unset config topology
        dict unset config gaps
        return $config
    }
    :method historyDrop {db where reason} {
        set stats [lindex [:sql $db \
                               "SELECT count(*) AS n,coalesce(min(at),0) AS first,coalesce(max(at),0) AS last FROM ns_stats_samples WHERE $where"] 0]
        set n [dict get $stats n]
        if {!$n} {
            return
        }
        set first [dict get $stats first]
        set last [dict get $stats last]
        set now [clock microseconds]
        :sql $db "DELETE FROM ns_stats_samples WHERE $where"
        :sql $db {DELETE FROM ns_stats_topology WHERE id NOT IN (SELECT DISTINCT topology FROM ns_stats_samples)}
        :sql $db "INSERT INTO ns_stats_events(at,first,last,n,reason) VALUES($now,$first,$last,$n,[:quote $reason])"
        set prior [:scalar $db {SELECT value FROM ns_stats_meta WHERE key='droppedThrough'}]
        set through [expr {$prior eq "" ? $last : max($last,$prior)}]
        :sql $db "INSERT OR REPLACE INTO ns_stats_meta VALUES('droppedThrough','$through')"
    }

    :public method "history save" {} {
        #
        #   Persist the latest published sample if history is enabled, no write
        #   is in progress and its timestamp has not already been saved.
        #   Claim the write under the shared coordination lock, then perform
        #   database operations outside that lock.
        #
        #   Store the compressed sample and associated topology in one
        #   transaction, deduplicating samples by timestamp. Apply age and
        #   storage-budget eviction, retain coverage events and remove topology
        #   no longer referenced by retained samples.
        #
        #   On failure, roll back the transaction, retain the pending sample
        #   for retry and update failure status. Repeated identical warnings
        #   are throttled. A subsequent successful write records the failed
        #   interval for coverage reporting.
        #
        #   Release the database handle and clear the write claim on completion.
        #   Does not collect observations or advance the sampling baseline;
        #   callers inspect history status for the persistence outcome.
        #
        set config {}
        set generation {}
        set stamp {}
        :locked {
            set candidate [nsv_get ns_stats history]
            set sample [nsv_get ns_stats pair]
            if {$candidate ne "" && [dict get $candidate enabled] && ![dict get $candidate busy] && $sample ne ""} {
                set stamp [dict get $sample timestamp]
                if {$stamp ne [dict get $candidate lastSaved]} {
                    set config $candidate
                    set generation [dict get $config generation]
                    dict set candidate busy true
                    nsv_set ns_stats history $candidate
                }
            }
        }
        if {$config eq ""} {
            return
        }
        set db {}
        set transaction false
        set ok false
        set errorMessage {}
        try {
            # Collection and SQLite work happen outside the shared control mutex.
            set now [clock seconds]
            set topology [dict get $config topology]
            if {$topology eq "" || $now-[dict get $config topologyAt]>=60} {
                set topology [:describe]
                foreach k {pid bootTime uptimeSeconds} {dict unset topology process $k}
                dict set config topology $topology
                dict set config topologyAt $now
            }
            # Stable Tcl serialization is enough for a private cache key; store
            # the full topology in the table to detect the unlikely CRC collision.
            set topologyId [format %08x [zlib crc32 [encoding convertto utf-8 $topology]]]
            set record $sample
            dict unset record raw
            dict unset record cpu
            dict set record topologyId $topologyId
            set payload [encoding convertto utf-8 $record]
            set rawSize [string length $payload]
            if {$rawSize > 1048576} {
                error "history sample exceeds 1 MiB"
            }
            set packed [:pack $record]
            set packedTopology [:pack $topology]
            set size [expr {[string length $packed]+[string length $packedTopology]}]
            set db [:historyConnect $config]
            :historySchema $db
            :sql $db {BEGIN IMMEDIATE}
            set transaction true
            set cutoff [expr {([clock seconds]-[dict get $config retention])*1000000}]
            :historyDrop $db "at < $cutoff" age
            set page [:scalar $db {PRAGMA page_size}]
            set max [:scalar $db {PRAGMA max_page_count}]
            while {1} {
                set used [expr {([:scalar $db {PRAGMA page_count}] - [:scalar $db {PRAGMA freelist_count}]) * $page}]
                if {$used+3*$size < $max*$page*0.8} {
                    break
                }
                if {![:scalar $db {SELECT count(*) FROM ns_stats_samples}]} {
                    error "history metadata or sample exceeds storage budget"
                }
                :historyDrop $db {at IN (SELECT at FROM ns_stats_samples ORDER BY at LIMIT 100)} size
            }
            set stored [:scalar $db "SELECT payload FROM ns_stats_topology WHERE id=[:quote $topologyId]"]
            if {$stored ne "" && [:unpack $stored] ne $topology} {
                error "history topology fingerprint collision"
            }
            :sql $db "INSERT OR IGNORE INTO ns_stats_topology VALUES([:quote $topologyId],[:quote $packedTopology])"
            :sql $db "INSERT OR IGNORE INTO ns_stats_samples VALUES($stamp,[:quote [dict get $record instance]],[:quote $topologyId],[:quote $packed])"
            foreach gap [dict get $config gaps] {
                lassign $gap first last n
                :sql $db "INSERT INTO ns_stats_events(at,first,last,n,reason) VALUES($stamp,$first,$last,$n,'write-failure')"
            }
            :sql $db {DELETE FROM ns_stats_events WHERE id NOT IN (SELECT id FROM ns_stats_events ORDER BY id DESC LIMIT 64)}
            :sql $db {DELETE FROM ns_stats_topology WHERE id NOT IN (SELECT DISTINCT topology FROM ns_stats_samples)}
            :sql $db {COMMIT}
            set transaction false
            set ok true

        } on error {message options} {
            set errorMessage $message
            if {$transaction} {
                catch {:sql $db {ROLLBACK}}
            }
            if {$message ne [dict get $config lastFailure] || [clock seconds]-[dict get $config lastLoggedAt] >= 60} {
                ns_log warning "ns_stats: history write failed: $message"
                dict set config lastLoggedAt [clock seconds]
            }
        } finally {
            if {$db ne ""} {
                ns_db releasehandle $db
            }
            :locked {
                set current [nsv_get ns_stats history]
                if {$generation eq [dict get $current generation]} {
                    dict set current busy false
                    dict set current lastLoggedAt [dict get $config lastLoggedAt]
                    if {$ok} {
                        dict set current lastSaved $stamp
                        dict set current lastFailure {}
                        dict set current gaps {}
                        dict set current topology [dict get $config topology]
                        dict set current topologyAt [dict get $config topologyAt]
                    } else {
                        dict incr current failures
                        dict set current lastFailure $errorMessage
                        set gaps [dict get $current gaps]
                        if {$gaps eq ""} {
                            set gaps [list [list $stamp $stamp 1]]
                        } else {
                            lassign [lindex $gaps 0] first last n
                            set gaps [list [list $first $stamp [expr {$n+1}]]]
                        }
                        dict set current gaps $gaps
                    }
                    nsv_set ns_stats history $current
                }
            }
        }
    }

    :method historyWindowCoverage {db low high} {
        # Inspect the whole window on the final page, including its boundary
        # neighbors. Read compressed payloads in bounded batches so coverage
        # does not depend on client-carried state or retain all snapshots.
        set previous {}
        set prior [:scalar $db "SELECT payload FROM ns_stats_samples WHERE at < $low ORDER BY at DESC LIMIT 1"]
        if {$prior ne ""} {
            set previous [:unpack $prior]
        }
        set gaps {}
        set restarts {}
        set resets 0
        set cursor [expr {$low-1}]
        set finished false
        while {!$finished} {
            set rows [:sql $db "SELECT at,payload FROM ns_stats_samples WHERE at > $cursor ORDER BY at LIMIT 128"]
            if {$rows eq ""} {
                break
            }
            foreach row $rows {
                set cursor [dict get $row at]
                set record [:unpack [dict get $row payload]]
                if {$cursor<$high && ![dict get $record sampling ready]} {
                    incr resets
                }
                if {$previous ne ""} {
                    if {$cursor<$high && [dict get $previous instance] ne [dict get $record instance]} {
                        lappend restarts [dict get $record sampling to]
                    }
                    set delta [expr {($cursor-[dict get $previous timestamp])/1000000.0}]
                    if {$delta>1.5*max([dict get $previous sampling intervalSeconds],
                                       [dict get $record sampling intervalSeconds])
                        || ($cursor>=$high && (![dict get $record sampling ready]
                            || [dict get $previous instance] ne [dict get $record instance]))} {
                        lappend gaps [dict create from [dict get $previous sampling to] to [dict get $record sampling to]]
                    }
                }
                if {$cursor >= $high} {
                    set finished true
                    break
                }
                set previous $record
            }
        }
        return [dict create gaps $gaps restartBoundaries $restarts resetSamples $resets]
    }

    :method historyStorage {db config} {
        # Metadata only: no sample decompression. Allocated bytes include free
        # SQLite pages; used bytes exclude those pages. The configured budget
        # also reserves journal headroom, so report the effective database cap.
        set bounds [lindex [:sql $db {SELECT count(*) AS n,coalesce(min(at),0) AS first,coalesce(max(at),0) AS last FROM ns_stats_samples}] 0]
        set first [dict get $bounds first]
        set last [dict get $bounds last]
        set pageSize [:scalar $db {PRAGMA page_size}]
        set pageCount [:scalar $db {PRAGMA page_count}]
        set freePages [:scalar $db {PRAGMA freelist_count}]
        set used [expr {($pageCount-$freePages)*$pageSize}]
        set span [expr {($last-$first)/1000000.0}]
        return [dict create retainedFrom [expr {$first>0 ? $first/1000000.0 : ""}] \
                    retainedTo [expr {$last>0 ? $last/1000000.0 : ""}] \
                    retainedSeconds $span sampleCount [dict get $bounds n] \
                    pageSize $pageSize allocatedBytes [expr {$pageCount*$pageSize}] \
                    usedBytes $used freeBytes [expr {$freePages*$pageSize}] \
                    databaseLimitBytes [expr {[:scalar $db {PRAGMA max_page_count}]*$pageSize}] \
                    maxBytes [dict get $config maxBytes] \
                    bytesPerSample [expr {[dict get $bounds n]>0 ? double($used)/[dict get $bounds n] : ""}] \
                    bytesPerSecond [expr {$span>0 ? $used/$span : ""}]]
    }

    :public method "history storage" {} {
        # Return retained bounds/count and SQLite space usage for sizing the
        # dedicated history database. Run in the history owner server. Does
        # not collect samples, write history or change its configuration.
        set config [nsv_get ns_stats history]
        if {$config eq ""} {
            error "history is not configured"
        }
        if {[dict get $config owner] ne [ns_info server]} {
            error "query history from its configured owner server"
        }
        set db [:historyConnect $config]
        try {
            :sql $db {BEGIN}
            try {
                set result [:historyStorage $db $config]
            } finally {
                :sql $db {ROLLBACK}
            }
            return $result
        } finally {
            ns_db releasehandle $db
        }
    }

    :public method "history query" {
        -from:integer,required
        -to:integer,required
        {-server {}}
        {-limit:integer 1000}
        {-cursor:integer 0}
    } {
        #
        #   Return stored samples, associated topology, paging information and
        #   coverage for an epoch interval. Queries run in the configuring
        #   virtual server and use an operation-local database handle.
        #   Historical server selection may include servers no longer active.
        #
        if {$from < 0
            || $to <= $from
            || $to - $from > 86400
            || $to > [clock seconds]
            || $limit < 1
            || $limit > 1000
            || $cursor < 0
        } {
            error "expected an epoch window up to 24 hours, limit 1..1000 and nonnegative cursor"
        }
        if {[string length $server]>512 || [string first \x00 $server] >= 0} {
            error "invalid server name"
        }
        set config [nsv_get ns_stats history]
        if {$config eq ""} {
            error "history is not configured"
        }
        if {[dict get $config owner] ne [ns_info server]} {
            error "query history from its configured owner server"
        }
        set db [:historyConnect $config]
        set transaction false
        try {
            :sql $db {BEGIN}
            set transaction true
            set low [expr {$from * 1000000}]
            set high [expr {$to * 1000000}]
            set count [expr {$limit + 1}]
            set rows [:sql $db "SELECT at,topology,payload FROM ns_stats_samples WHERE at >= $low AND at < $high AND at > $cursor ORDER BY at LIMIT $count"]
            set more [expr {[llength $rows] > $limit}]
            set rows [lrange $rows 0 [expr {$limit - 1}]]
            set records {}
            set gaps {}
            set restarts {}
            set previous {}
            set topology {}
            set resets 0
            set boundary [expr {max($low,$cursor)}]
            set operator [expr {$cursor >= $low ? "<=" : "<"}]
            set prior [:scalar $db "SELECT payload FROM ns_stats_samples WHERE at $operator $boundary ORDER BY at DESC LIMIT 1"]

            if {$prior ne ""} {
                set previous [:unpack $prior]
            }
            foreach row $rows {
                set record [:unpack [dict get $row payload]]
                if {![dict get $record sampling ready]} {
                    incr resets
                }
                if {$previous ne ""} {
                    if {[dict get $previous instance] ne [dict get $record instance]} {
                        lappend restarts [dict get $record sampling to]
                    }
                    set delta [expr {([dict get $record timestamp]-[dict get $previous timestamp]) / 1000000.0}]
                    if {$delta > 1.5 * max([dict get $previous sampling intervalSeconds],[dict get $record sampling intervalSeconds])} {
                        lappend gaps [dict create from [dict get $previous sampling to] to [dict get $record sampling to]]
                    }
                }
                set previous $record
                set id [dict get $row topology]
                dict set record topologyId $id
                if {![dict exists $topology $id]} {
                    dict set topology $id [:selectServer \
                                               [:unpack [:scalar $db "SELECT payload FROM ns_stats_topology WHERE id=[:quote $id]"]] \
                                               $server false]
                }
                lappend records [:selectServer $record $server false]
            }

            if {!$more && $previous ne ""} {
                set following [:scalar $db "SELECT payload FROM ns_stats_samples WHERE at >= $high ORDER BY at LIMIT 1"]
                if {$following ne ""} {
                    set nextRecord [:unpack $following]
                    set delta [expr {([dict get $nextRecord timestamp]-[dict get $previous timestamp]) / 1000000.0}]
                    if {$delta > 1.5 * max([dict get $previous sampling intervalSeconds],[dict get $nextRecord sampling intervalSeconds])} {
                        lappend gaps [dict create from [dict get $previous sampling to] to [dict get $nextRecord sampling to]]
                    }
                }
            }
            set bounds [lindex [:sql $db {SELECT coalesce(min(at),0) AS first,coalesce(max(at),0) AS last FROM ns_stats_samples}] 0]
            set events [:sql $db {SELECT at,first,last,n,reason FROM ns_stats_events ORDER BY id DESC}]
            set first [dict get $bounds first]
            set last [dict get $bounds last]
            set overlap false
            foreach event $events {
                if {[dict get $event first] < $high && [dict get $event last] >= $low} {
                    set overlap true
                }
            }
            set through [:scalar $db {SELECT value FROM ns_stats_meta WHERE key='droppedThrough'}]
            if {$through ne "" && $through >= $low} {
                set overlap true
            }
            set next {}
            if {$more} {
                set next [dict get [lindex $rows end] at]
            }
            # Intermediate coverage describes this page. The final response
            # examines all retained intervals in the requested window again,
            # within this read transaction, independently of the paging cursor.
            if {!$more} {
                set window [:historyWindowCoverage $db $low $high]
                set gaps [dict get $window gaps]
                set restarts [dict get $window restartBoundaries]
                set resets [dict get $window resetSamples]
            }
            set complete [expr {$first>0 && $first<=$low && $last>=$high && !$more
                                && !$overlap && $gaps eq "" && $restarts eq "" && $resets==0}]
            set reasons {}
            if {$more} {lappend reasons more-pages}
            if {$first==0 || $first>$low} {lappend reasons before-retained-history}
            if {$last<$high} {lappend reasons after-latest-sample}
            if {$overlap} {lappend reasons eviction-or-write-failure}
            if {$gaps ne ""} {lappend reasons gaps}
            if {$restarts ne ""} {lappend reasons restarts}
            if {$resets>0} {lappend reasons baseline-or-reset}
            set result [dict create \
                            schemaVersion 1 from $from to $to records $records \
                            topologies $topology nextCursor $next more $more \
                            storage [:historyStorage $db $config] \
                            coverage [dict create \
                                          complete $complete \
                                          windowExamined [expr {!$more}] \
                                          scope [expr {$more ? "page" : "window"}] \
                                          incompleteReasons $reasons \
                                          retainedFrom [expr {$first / 1000000.0}] \
                                          retainedTo [expr {$last / 1000000.0}] \
                                          gaps $gaps \
                                          restartBoundaries $restarts \
                                          resetSamples $resets \
                                          events $events \
                                          evictionOrFailureOverlap $overlap \
                                          droppedThrough $through \
                                          writeFailuresSinceStart [dict get $config failures]]]
            :sql $db {COMMIT}
            set transaction false
            return $result
        } finally {
            if {$transaction} {
                catch {:sql $db {ROLLBACK}}
            }
            ns_db releasehandle $db
        }
    }
}

nx::Class create ::ns::ns_stats -superclass ::ns::stats::Sampler {
    #
    #   Provide the public ns_stats object command. Inherited NX ensemble
    #   methods implement sampling and validate their options directly.
    #   SQLiteHistory supplies the history ensemble as an object mixin;
    #   persistence remains inactive until history configure is called.
    #
    :method startup {} {
        #
        #   Apply the process-wide ns/stats startup configuration once, in
        #   the selected server's startup callback. Database modules and pool
        #   permissions are available at this point. Reuse the public runtime
        #   commands for validation, scheduling and persistence initialization.
        #   Configuration is not reapplied by sampling or history callbacks.
        #
        set section ns/stats
        set server [ns_config $section server [lindex [ns_info servers] 0]]
        if {$server ne [ns_info server]} {
            return
        }
        set sampling [ns_config -bool $section sampling false]
        set pool [ns_config $section historyPool {}]
        if {!$sampling && $pool eq ""} {
            return
        }
        set interval [ns_baseunit -time [ns_config $section samplingInterval 10s]]
        if {$sampling && ($interval<1 || $interval>3600)} {
            error "ns/stats samplingInterval must be 1..3600 seconds"
        }
        if {$pool ne ""} {
            :history configure -pool $pool \
                -retention [ns_config $section historyRetention 24h] \
                -maxBytes [ns_config $section historyMaxBytes 64MiB]
        }
        if {$sampling} {
            :sampling start -interval $interval
        }
    }

    :public method "history persist" {} {
        #
        #   Scheduled persistence entry point. Restore the configured history
        #   mixins in this interpreter, then save the latest published sample.
        #   Return without database access when persistence is disabled.
        #
        set config [nsv_get ns_stats history]
        if {$config eq "" || ![dict get $config enabled]} {
            return
        }
        :object mixins set [dict get $config mixins]
        :history save
    }
}

# The serializer normally excludes classes directly in ::ns, reserving that
# namespace for NaviServer internals. Export this interface class explicitly.
if {[info commands ::nx::serializer::Serializer] ne ""} {
    ::nx::serializer::Serializer exportObjects ::ns::ns_stats
}

# Only definitions and the configured interface object enter the blueprint;
# database handles and request-local state are acquired per operation.
if {![nsf::object::exists ::ns_stats]} {
    ::ns::ns_stats create ::ns_stats
    ::ns_stats object mixins add ::ns::stats::SQLiteHistory
}

# Initialize only in the selected server, after all modules have loaded.
# Sourcing definitions after startup must not reapply the startup settings.
if {![ns_info started]} {
    set statsStartupServer [ns_config ns/stats server [lindex [ns_info servers] 0]]
    if {$statsStartupServer ni [ns_info servers]} {
        error "ns/stats server '$statsStartupServer' is not a configured virtual server"
    }
    if {$statsStartupServer eq [ns_info server]} {
        ns_atstartup {::nsf::dispatch ::ns_stats startup}
    }
    unset statsStartupServer
}
