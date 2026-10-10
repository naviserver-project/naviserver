#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#

namespace eval ::ns {}

if {[info commands ::nx::Class] eq ""} {
    ns_log warning "NSF is not installed. The commands ::ns_stats* are not available"
    return
}

# create namespace and global nsv
namespace eval ::ns::stats {
    nsv_array create -scope process ns_stats
    foreach key {control pair history} {
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
                        queues {} \
                        servers {} \
                        cpu {} \
                        availability {} \
                        units {time seconds cpuCounter microseconds cpuPercent percentOfOneCore}]
        if {![catch {ns_driver stats} stats]} {
            foreach entry $stats {
                dict set result drivers [dict get $entry thread] $entry
            }
            dict set result availability drivers true
        } else {
            dict set result availability drivers false
        }
        foreach kind {driver writer spooler} {
            if {![catch {ns_driver queues $kind} entries]} {
                foreach entry $entries {
                    dict set result queues $kind [dict get $entry thread] $entry
                }
                dict set result availability $kind true
            } else {
                dict set result availability $kind false
            }
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

    :public method "sampling start" {{-interval:integer 10}} {
        #   Start the process-wide sampling callback at the requested interval
        #   in seconds (default 10, range 1..3600). Repeated calls with the
        #   same running interval reuse the scheduler; a different interval
        #   requires stopping the sampler first.
        #
        #   Clears the previous sample and baseline when starting a stopped
        #   sampler. Returns the resulting status dictionary. Persistence
        #   must be enabled separately.

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

    :public method sample {{-server {}}} {
        #
        #   Return the latest published sample, updating its reported age and
        #   running state. Before any publication, return a dictionary with
        #   sampling readiness false and no interval observations.
        #
        #   An optional server argument restricts virtual-server data; CPU
        #   measurements remain process-wide. Does not collect observations
        #   or advance the baseline. The internal raw baseline is omitted
        #   from the returned dictionary.
        #
        if {$server ne "" && $server ni [ns_info servers]} {
            error "unknown server: $server"
        }
        set result [nsv_get ns_stats pair]
        if {$result eq ""} {
            return [dict create schemaVersion 1 \
                        sampling {ready false from {} to {} elapsedSeconds {} ageSeconds {} reset false} \
                        servers {} availability {}]
        }
        dict set result sampling running [dict get [nsv_get ns_stats control] running]
        dict set result sampling ageSeconds [expr {max(0,([clock microseconds]-[dict get $result timestamp])/1000000.0)}]
        dict unset result raw
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
                    if {$k ni {requests queued spooled dropped queuetime filtertime runtime tracetime}} {
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
                if {$ready && [dict exists $entry $k] && [dict exists $previous drivers $thread $k]} {
                    set d [:delta [dict get $entry $k] [dict get $previous drivers $thread $k]]
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
        dict set result cpuPercent [dict create \
                                        scope process \
                                        matchedThreads $matched \
                                        observedThreads [dict size [dict get $current cpu]] \
                                        groups $totals \
                                        threads $percentages]
        return $result
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

# Small nsdb adapter. All SQL is fixed; values are quoted separately. Only
# non-null selected columns are fetched (including aggregate coalesce), since
# older nsdbsqlite drivers cannot return SQL NULL safely.
proc ::ns::stats::quote {value} {
    if {[string first \x00 $value] >= 0} {
        return "CAST(X'[binary encode hex [encoding convertto utf-8 $value]]' AS TEXT)"
    }
    return "'[string map [list ' ''] $value]'"
}
proc ::ns::stats::sql {db query} {
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
proc ::ns::stats::pack {value} {binary encode hex [zlib compress [encoding convertto utf-8 $value] 1]}
proc ::ns::stats::unpack {value} {encoding convertfrom utf-8 [zlib decompress [binary decode hex $value]]}
proc ::ns::stats::scalar {db query} {lindex [dict values [lindex [::ns::stats::sql $db $query] 0]] 0}

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
            if {[::ns::stats::scalar $db {
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
                ::ns::stats::sql $db $query
            }
            set page [::ns::stats::scalar $db {PRAGMA page_size}]
            set maximum [expr {int([dict get $config maxBytes]*0.45)/$page}]

            if {[::ns::stats::scalar $db {PRAGMA page_count}]>$maximum} {
                error "existing history database exceeds byte budget"
            }
            ::ns::stats::sql $db "PRAGMA max_page_count=$maximum"
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
            ::ns::stats::sql $db $query
        }
        set version [::ns::stats::scalar $db {SELECT value FROM ns_stats_meta WHERE key='schemaVersion'}]
        if {$version ne "" && $version ne "1"} {
            error "unsupported diagnostics history schema"
        }
        set source [::ns::stats::quote [list [ns_info hostname] [ns_info home]]]
        set stored [::ns::stats::scalar $db {SELECT value FROM ns_stats_meta WHERE key='source'}]
        if {$stored ne "" && [::ns::stats::quote $stored] ne $source} {
            error "history database belongs to another process installation"
        }
        ::ns::stats::sql $db {INSERT OR IGNORE INTO ns_stats_meta VALUES('schemaVersion','1')}
        ::ns::stats::sql $db {INSERT OR IGNORE INTO ns_stats_meta VALUES('encoding','tcl-zlib-hex-v1')}
        ::ns::stats::sql $db "INSERT OR IGNORE INTO ns_stats_meta VALUES('source',$source)"
    }

    :public method "history configure" {
        -pool:required
        {-retention:integer 86400}
        {-maxBytes:integer 67108864}
    } {
        #
        #   Validate the database pool and retention limits, initialize the
        #   schema and start a separate persistence callback. Repeated
        #   configuration with identical settings is harmless; changing
        #   active settings requires disabling persistence first.
        #   Return the resulting history status. Does not start sampling.
        #
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
        set stats [lindex [::ns::stats::sql $db \
                               "SELECT count(*) AS n,coalesce(min(at),0) AS first,coalesce(max(at),0) AS last FROM ns_stats_samples WHERE $where"] 0]
        set n [dict get $stats n]
        if {!$n} {
            return
        }
        set first [dict get $stats first]
        set last [dict get $stats last]
        set now [clock microseconds]
        ::ns::stats::sql $db "DELETE FROM ns_stats_samples WHERE $where"
        ::ns::stats::sql $db {DELETE FROM ns_stats_topology WHERE id NOT IN (SELECT DISTINCT topology FROM ns_stats_samples)}
        ::ns::stats::sql $db "INSERT INTO ns_stats_events(at,first,last,n,reason) VALUES($now,$first,$last,$n,[::ns::stats::quote $reason])"
        set prior [::ns::stats::scalar $db {SELECT value FROM ns_stats_meta WHERE key='droppedThrough'}]
        set through [expr {$prior eq "" ? $last : max($last,$prior)}]
        ::ns::stats::sql $db "INSERT OR REPLACE INTO ns_stats_meta VALUES('droppedThrough','$through')"
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
            set packed [::ns::stats::pack $record]
            set packedTopology [::ns::stats::pack $topology]
            set size [expr {[string length $packed]+[string length $packedTopology]}]
            set db [:historyConnect $config]
            :historySchema $db
            ::ns::stats::sql $db {BEGIN IMMEDIATE}
            set transaction true
            set cutoff [expr {([clock seconds]-[dict get $config retention])*1000000}]
            :historyDrop $db "at < $cutoff" age
            set page [::ns::stats::scalar $db {PRAGMA page_size}]
            set max [::ns::stats::scalar $db {PRAGMA max_page_count}]
            while {1} {
                set used [expr {([::ns::stats::scalar $db {PRAGMA page_count}] - [::ns::stats::scalar $db {PRAGMA freelist_count}]) * $page}]
                if {$used+3*$size < $max*$page*0.8} {
                    break
                }
                if {![::ns::stats::scalar $db {SELECT count(*) FROM ns_stats_samples}]} {
                    error "history metadata or sample exceeds storage budget"
                }
                :historyDrop $db {at IN (SELECT at FROM ns_stats_samples ORDER BY at LIMIT 100)} size
            }
            set stored [::ns::stats::scalar $db "SELECT payload FROM ns_stats_topology WHERE id=[::ns::stats::quote $topologyId]"]
            if {$stored ne "" && [::ns::stats::unpack $stored] ne $topology} {
                error "history topology fingerprint collision"
            }
            ::ns::stats::sql $db "INSERT OR IGNORE INTO ns_stats_topology VALUES([::ns::stats::quote $topologyId],[::ns::stats::quote $packedTopology])"
            ::ns::stats::sql $db "INSERT OR IGNORE INTO ns_stats_samples VALUES($stamp,[::ns::stats::quote [dict get $record instance]],[::ns::stats::quote $topologyId],[::ns::stats::quote $packed])"
            foreach gap [dict get $config gaps] {
                lassign $gap first last n
                ::ns::stats::sql $db "INSERT INTO ns_stats_events(at,first,last,n,reason) VALUES($stamp,$first,$last,$n,'write-failure')"
            }
            ::ns::stats::sql $db {DELETE FROM ns_stats_events WHERE id NOT IN (SELECT id FROM ns_stats_events ORDER BY id DESC LIMIT 64)}
            ::ns::stats::sql $db {DELETE FROM ns_stats_topology WHERE id NOT IN (SELECT DISTINCT topology FROM ns_stats_samples)}
            ::ns::stats::sql $db {COMMIT}
            set transaction false
            set ok true

        } on error {message options} {
            set errorMessage $message
            if {$transaction} {
                catch {::ns::stats::sql $db {ROLLBACK}}
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
            ::ns::stats::sql $db {BEGIN}
            set transaction true
            set low [expr {$from * 1000000}]
            set high [expr {$to * 1000000}]
            set count [expr {$limit + 1}]
            set rows [::ns::stats::sql $db "SELECT at,topology,payload FROM ns_stats_samples WHERE at >= $low AND at < $high AND at > $cursor ORDER BY at LIMIT $count"]
            set more [expr {[llength $rows]>$limit}]
            set rows [lrange $rows 0 [expr {$limit - 1}]]
            set records {}
            set gaps {}
            set restarts {}
            set previous {}
            set topology {}
            set resets 0
            set boundary [expr {max($low,$cursor)}]
            set operator [expr {$cursor >= $low ? "<=" : "<"}]
            set prior [::ns::stats::scalar $db "SELECT payload FROM ns_stats_samples WHERE at $operator $boundary ORDER BY at DESC LIMIT 1"]

            if {$prior ne ""} {
                set previous [::ns::stats::unpack $prior]
            }
            foreach row $rows {
                set record [::ns::stats::unpack [dict get $row payload]]
                if {![dict get $record sampling ready]} {
                    incr resets
                }
                if {$previous ne ""} {
                    if {[dict get $previous instance] ne [dict get $record instance]} {
                        lappend restarts [dict get $record sampling to]
                    }
                    set delta [expr {([dict get $record timestamp]-[dict get $previous timestamp])/1000000.0}]
                    if {$delta > 1.5 * max([dict get $previous sampling intervalSeconds],[dict get $record sampling intervalSeconds])} {
                        lappend gaps [dict create from [dict get $previous sampling to] to [dict get $record sampling to]]
                    }
                }
                set previous $record
                set id [dict get $row topology]
                dict set record topologyId $id
                if {![dict exists $topology $id]} {
                    dict set topology $id [:selectServer \
                                               [::ns::stats::unpack [::ns::stats::scalar $db "SELECT payload FROM ns_stats_topology WHERE id=[::ns::stats::quote $id]"]] \
                                               $server false]
                }
                lappend records [:selectServer $record $server false]
            }

            if {!$more && $previous ne ""} {
                set following [::ns::stats::scalar $db "SELECT payload FROM ns_stats_samples WHERE at >= $high ORDER BY at LIMIT 1"]
                if {$following ne ""} {
                    set nextRecord [::ns::stats::unpack $following]
                    set delta [expr {([dict get $nextRecord timestamp]-[dict get $previous timestamp])/1000000.0}]
                    if {$delta > 1.5 * max([dict get $previous sampling intervalSeconds],[dict get $nextRecord sampling intervalSeconds])} {
                        lappend gaps [dict create from [dict get $previous sampling to] to [dict get $nextRecord sampling to]]
                    }
                }
            }
            set bounds [lindex [::ns::stats::sql $db {SELECT coalesce(min(at),0) AS first,coalesce(max(at),0) AS last FROM ns_stats_samples}] 0]
            set events [::ns::stats::sql $db {SELECT at,first,last,n,reason FROM ns_stats_events ORDER BY id DESC}]
            set first [dict get $bounds first]
            set last [dict get $bounds last]
            set overlap false
            foreach event $events {if {[dict get $event first]<$high && [dict get $event last]>=$low} {set overlap true}}
            set through [::ns::stats::scalar $db {SELECT value FROM ns_stats_meta WHERE key='droppedThrough'}]
            if {$through ne "" && $through >= $low} {
                set overlap true
            }
            set next {}
            if {$more} {
                set next [dict get [lindex $rows end] at]
            }
            set complete [expr {$first > 0 && $first <= $low && $last >= $high && !$more && $cursor == 0
                                && !$overlap && $gaps eq "" && $restarts eq "" && $resets == 0}]
            set result [dict create \
                            schemaVersion 1 from $from to $to records $records \
                            topologies $topology nextCursor $next more $more \
                            coverage [dict create \
                                          complete $complete \
                                          windowExamined [expr {!$more && $cursor == 0}] \
                                          retainedFrom [expr {$first / 1000000.0}] \
                                          retainedTo [expr {$last / 1000000.0}] \
                                          gaps $gaps \
                                          restartBoundaries $restarts \
                                          resetSamples $resets \
                                          events $events \
                                          evictionOrFailureOverlap $overlap \
                                          droppedThrough $through \
                                          writeFailuresSinceStart [dict get $config failures]]]
            ::ns::stats::sql $db {COMMIT}
            set transaction false
            return $result
        } finally {
            if {$transaction} {
                catch {::ns::stats::sql $db {ROLLBACK}}
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
