# Optional benchmark using the dedicated ns_stats_test pool from test.nscfg.
# Run in command mode after loading the updated ns_stats.tcl. Output is CSV.
# It never selects the production history pool. Sample collection, source-file
# reset and schema setup are outside the timed save/read calls.
set pool ns_stats_test
if {[info commands ns_db] eq {} || $pool ni [ns_db pools]} {
    error "benchmark requires the dedicated ns_stats_test pool"
}
set path [ns_config ns/db/pool/$pool datasource]
if {[file tail $path] ne "ns-stats-[pid].db"} {
    error "benchmark requires the disposable test.nscfg datasource"
}
set output /tmp/ns-stats-history-policy-benchmark.csv
if {[info exists ::env(NS_STATS_BENCHMARK_OUTPUT)]} {
    set output $::env(NS_STATS_BENCHMARK_OUTPUT)
}
set seedRows 4096
if {[info exists ::env(NS_STATS_BENCHMARK_SEED_ROWS)]} {
    set seedRows $::env(NS_STATS_BENCHMARK_SEED_ROWS)
}
if {![string is integer -strict $seedRows] || $seedRows<0 || $seedRows>8192} {
    error "NS_STATS_BENCHMARK_SEED_ROWS must be 0..8192"
}
set ::historyBenchActive false
array set ::historyBenchTimes {connect 0 commit 0}
nx::Class create ::HistoryPolicyBenchmark {
    :method historyConnect {config} {
        set started [clock clicks -microseconds]
        try {
            next
        } finally {
            if {$::historyBenchActive} {
                incr ::historyBenchTimes(connect) [expr {[clock clicks -microseconds]-$started}]
            }
        }
    }
    :method sql {db query} {
        set started [clock clicks -microseconds]
        try {
            next
        } finally {
            if {$::historyBenchActive && [string toupper [string trim $query]] eq "COMMIT"} {
                incr ::historyBenchTimes(commit) [expr {[clock clicks -microseconds]-$started}]
            }
        }
    }
}
::ns_stats object mixins add ::HistoryPolicyBenchmark
set out [open $output w]
try {
    chan puts $out {trial,journal,synchronous,seedRows,records,saveMeanUs,connectMeanUs,commitMeanUs,queryMeanUs,poolWaitMs,databaseBytes,journalBytes,sqliteVersion}
    set trial 0
    foreach order {
        {{DELETE FULL} {DELETE NORMAL} {WAL FULL} {WAL NORMAL}}
        {{WAL NORMAL} {WAL FULL} {DELETE NORMAL} {DELETE FULL}}
        {{DELETE NORMAL} {WAL NORMAL} {DELETE FULL} {WAL FULL}}
    } {
        incr trial
        foreach policy $order {
            lassign $policy journal synchronous
            ns_stats history disable
            ns_stats sampling stop
            ns_db bouncepool $pool
            foreach suffix {{} -journal -wal -shm} {
                file delete -force $path$suffix
            }
            ns_set update [ns_configsection ns/db/pool/$pool/sqlite] journalMode $journal
            ns_set update [ns_configsection ns/db/pool/$pool/sqlite] synchronous $synchronous
            set status [ns_stats history configure -pool $pool -retention 24h -maxBytes 64MiB]
            # Keep scheduled persistence out of this controlled benchmark.
            ns_unschedule_proc [dict get $status scheduler]
            set config [nsv_get ns_stats history]
            dict set config scheduler {}
            nsv_set ns_stats history $config
            ns_stats sampling start -interval 3600
            ::ns_stats sampling tick
            set saved 0
            set saveTicks 0
            set readTicks 0
            set reads 0
            array set ::historyBenchTimes {connect 0 commit 0}
            set beforeWait [dict get [ns_db stats] $pool waittime]
            set baseStamp [expr {([clock seconds]-20)*1000000}]
            for {set i -5} {$i<200} {incr i} {
                ::ns_stats sampling tick
                # Keep every real sample payload in a fixed past query window.
                set sample [nsv_get ns_stats pair]
                dict set sample timestamp [expr {$baseStamp+($i+5)*10000}]
                nsv_set ns_stats pair $sample
                set ::historyBenchActive [expr {$i>=0}]
                set started [clock clicks -microseconds]
                ns_stats history save
                set elapsed [expr {[clock clicks -microseconds]-$started}]
                set ::historyBenchActive false
                if {[dict get [ns_stats history status] failures]!=0} {
                    error "history save failed"
                }
                if {$i==-1 && $seedRows>0} {
                    # Populate retained history outside timing. Repeated compressed
                    # payloads model storage volume, not historical observations.
                    set db [::nsf::dispatch ::ns_stats historyConnect [nsv_get ns_stats history]]
                    try {
                        ::nsf::dispatch ::ns_stats sql $db {PRAGMA synchronous=OFF}
                        set seedStart [expr {([clock seconds]-10000)*1000000}]
                        ::nsf::dispatch ::ns_stats sql $db "WITH RECURSIVE ids(i) AS (
                            VALUES(0) UNION ALL SELECT i+1 FROM ids WHERE i<[expr {$seedRows-1}]
                        ), template AS (
                            SELECT instance,topology,payload,allocator FROM ns_stats_samples ORDER BY at LIMIT 1
                        ) INSERT INTO ns_stats_samples(at,instance,topology,payload,allocator)
                          SELECT $seedStart+i*1000000,instance,topology,payload,allocator FROM ids,template"
                        ::nsf::dispatch ::ns_stats sql $db "PRAGMA synchronous=$synchronous"
                        ::nsf::dispatch ::ns_stats sql $db {PRAGMA wal_checkpoint(TRUNCATE)}
                    } finally {
                        ns_db releasehandle $db
                    }
                    set beforeWait [dict get [ns_db stats] $pool waittime]
                }
                if {$i>=0} {
                    incr saved
                    incr saveTicks $elapsed
                    if {$i%5==0} {
                        set now [clock seconds]
                        set started [clock clicks -microseconds]
                        set answer [ns_stats history query -from [expr {$now-60}] -to $now -limit 32]
                        incr readTicks [expr {[clock clicks -microseconds]-$started}]
                        if {![llength [dict get $answer records]]} {
                            error "benchmark history read returned no records"
                        }
                        incr reads
                    }
                }
            }
            set waitMs [expr {1000.0*([dict get [ns_db stats] $pool waittime]-$beforeWait)}]
            set db [::nsf::dispatch ::ns_stats historyConnect [nsv_get ns_stats history]]
            try {
                set version [::nsf::dispatch ::ns_stats scalar $db {SELECT sqlite_version()}]
                set count [::nsf::dispatch ::ns_stats scalar $db {SELECT count(*) FROM ns_stats_samples}]
                if {$count!=205+$seedRows} {
                    error "benchmark lost history samples: $count"
                }
            } finally {
                ns_db releasehandle $db
            }
            set journalBytes 0
            foreach suffix {-journal -wal -shm} {
                if {[file exists $path$suffix]} {
                    incr journalBytes [file size $path$suffix]
                }
            }
            chan puts $out [join [list $trial $journal $synchronous $seedRows $saved \
                [expr {$saveTicks/double($saved)}] [expr {$::historyBenchTimes(connect)/double($saved)}] \
                [expr {$::historyBenchTimes(commit)/double($saved)}] [expr {$readTicks/double($reads)}] \
                $waitMs [file size $path] $journalBytes $version] ,]
            flush $out
        }
    }
} finally {
    set ::historyBenchActive false
    ns_stats history disable
    ns_stats sampling stop
    ::ns_stats object mixins delete ::HistoryPolicyBenchmark
    ::HistoryPolicyBenchmark destroy
    close $out
    ns_db bouncepool $pool
    foreach suffix {{} -journal -wal -shm} {
        file delete -force $path$suffix
    }
}
ns_shutdown
vwait forever
