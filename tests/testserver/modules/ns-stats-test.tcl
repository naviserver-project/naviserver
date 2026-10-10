# Test endpoints use the standard testvhost listener and server mapping.
if {[ns_info server] eq "testvhost"} {
    proc ns_stats_test_sample {} {
        ns_stats sampling start -interval 3600
        ns_return 200 text/plain [list status [ns_stats sampling status] sample [ns_stats sample -server [ns_info server]]]
    }
    proc ns_stats_test_scope {} {
        switch -- [ns_queryget action] {
            share {
                nsv_array create -scope process ns_stats_test_scope
                nsv_dict set ns_stats_test_scope dict remote yes
                ns_return 200 text/plain [list [nsv_get ns_stats_test_scope value] [nsv_array scope ns_stats_test_scope] [nsv_names ns_stats_test_scope] [catch {nsv_array create ns_stats_test_scope}]]
            }
            conflict {
                nsv_set ns_stats_test_conflict value remote
                ns_return 200 text/plain ok
            }
            local {
                ns_return 200 text/plain [nsv_array exists ns_stats_test_local]
            }
            cleanup {
                nsv_unset -nocomplain ns_stats_test_conflict
                ns_return 200 text/plain ok
            }
            eval {
                for {set i 0} {$i < 100} {incr i} {
                    nsv_array eval ns_stats_test_scope {
                        set value [nsv_get ns_stats_test_scope count]
                        ns_sleep 0.0001
                        nsv_set ns_stats_test_scope count [expr {$value+1}]
                    }
                }
                ns_return 200 text/plain ok
            }
        }
    }
    ns_register_proc GET /ns-stats-test/sample ns_stats_test_sample
    ns_register_proc GET /ns-stats-test/scope ns_stats_test_scope
}
