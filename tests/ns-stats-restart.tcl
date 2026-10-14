package require tcltest
namespace import ::tcltest::*
test stats-startup-config {startup configuration enables sampling and persistence with normalized units} -returnCodes {ok error} -body {
    set sampling [ns_stats sampling status]
    set history [ns_stats history status]
    list [dict get $sampling running] [dict get $sampling interval] \
        [dict get $history enabled] [dict get $history owner] \
        [dict get $history retention] [dict get $history maxBytes]
} -result {true 3600 true primary 86400 4194304}
set db [ns_db gethandle diagnostics]
set oldInstances [::nsf::dispatch ::ns_stats sql $db {SELECT DISTINCT instance FROM ns_stats_samples}]
ns_db releasehandle $db
test stats-history-survives {previous process samples remain readable after restart} -returnCodes {ok error} -body {expr {[llength $oldInstances]>0}} -result 1
::ns_stats sampling tick
::ns_stats history save
set newInstance [dict get [ns_stats sampling status] instance]
test stats-new-instance {a new process lifetime has a distinct identity and no cross-process delta} -returnCodes {ok error} -body {
    list [expr {[dict get [lindex $oldInstances 0] instance] ne $newInstance}] [dict get [ns_stats sample] sampling ready]
} -result {1 false}
test stats-startup-runtime-override {runtime commands can change configured startup settings} -returnCodes {ok error} -body {
    ns_stats sampling stop
    set sampling [ns_stats sampling start -interval 1500ms]
    ns_stats history disable
    set history [ns_stats history configure -pool diagnostics -retention 2m -maxBytes 8MiB]
    list [dict get $sampling interval] [dict get $history retention] [dict get $history maxBytes]
} -result {1.5 120 8388608}
ns_stats history disable
ns_stats sampling stop
set failed $::tcltest::numTests(Failed)
cleanupTests
if {$failed} {ns_shutdown -restart} else {ns_shutdown}
vwait forever
