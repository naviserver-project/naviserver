package require tcltest
namespace import ::tcltest::*
ns_stats history configure -pool diagnostics -retention 86400 -maxBytes 4194304
set db [ns_db gethandle diagnostics]
set oldInstances [::ns::stats::sql $db {SELECT DISTINCT instance FROM ns_stats_samples}]
ns_db releasehandle $db
test stats-history-survives {previous process samples remain readable after restart} -body {expr {[llength $oldInstances]>0}} -result 1
ns_stats sampling start -interval 3600
::ns_stats sampling tick
::ns_stats history save
set newInstance [dict get [ns_stats sampling status] instance]
test stats-new-instance {a new process lifetime has a distinct identity and no cross-process delta} -body {
    list [expr {[dict get [lindex $oldInstances 0] instance] ne $newInstance}] [dict get [ns_stats sample] sampling ready]
} -result {1 false}
ns_stats history disable
ns_stats sampling stop
set failed $::tcltest::numTests(Failed)
cleanupTests
if {$failed} {ns_shutdown -restart} else {ns_shutdown}
vwait forever
