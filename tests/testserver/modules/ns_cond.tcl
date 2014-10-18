# -*- Tcl -*-

# These two files work together for testing ns_cond:
#   tests/testserver/modules/nscond.tcl
#   tests/ns_cond.test


proc tst_cond_shutdown {} {
    nsv_set {cond_test_1} shutdown_pending 1
}
ns_atshutdown {tst_cond_shutdown}

proc tst_cond_master {} {
    set proc_name {tst_cond_master}
    set aa {cond_test_1}
    set cond_id [ns_cond create]
    set ev_mut  [ns_mutex create {mutex_for_cond_test_1}]
    nsv_set $aa shutdown_pending 0
    nsv_set $aa {cond_id} $cond_id
    nsv_set $aa {ev_mut}  $ev_mut
    nsv_array set {ct1_done} [list]
    nsv_set {ct1_work_queue} 0 0

    set tid [ns_thread begindetached {
        # If necessary due to running this code in a different environment, you
        # can have the newly spawned worker thread first source this file here.
        tst_cond_worker
    }]
    ns_log Notice "$proc_name: New thread '$tid' started for running tst_cond_worker."

    # When we call ns_cond broadcast, the worker thread should immediately
    # wake up and process the new work.  If it does not something is wrong:
    foreach ww [list 1 2 3 4] {
        nsv_set {ct1_work_queue} $ww $ww
        ns_mutex lock $ev_mut
        ns_cond broadcast $cond_id
        ns_mutex unlock $ev_mut
        ns_sleep 1
    }

    set done_l [lsort -dictionary [nsv_array names {ct1_done}]]
    set todo_l [lsort -dictionary [nsv_array names {ct1_work_queue}]]
    set n_done [llength $done_l]
    set n_not  [llength $todo_l]
    ns_log Notice "$proc_name: $n_done work items done:  $done_l"
    ns_log Notice "$proc_name: $n_not work items NOT done:  $todo_l"

    return $n_done
}

# Note that calling ns_cond wait automatically unlocks the mutex, and
# waking up from ns_cond wait automatically locks the mutex.  See Rob
# Mayoff's 2002-02-22 notes and/or the man page for pthread_cond_wait.

proc tst_cond_worker {} {
    set proc_name {tst_cond_worker}
    set aa {cond_test_1}
    set cond_id [nsv_get $aa {cond_id}]
    set ev_mut  [nsv_get $aa {ev_mut}]

    while { [nsv_array exists $aa] && ![nsv_get $aa shutdown_pending] } {
        ns_mutex lock $ev_mut
        while { ![nsv_get $aa shutdown_pending] && [nsv_array size {ct1_work_queue}] < 1 } {
            # We are NOT ready to do more work yet, so wait on the condition code:
            if { [ns_cond wait $cond_id $ev_mut] } {
                ns_log Notice "$proc_name: Event '$cond_id' - got it."
            } else {
                ns_log Notice "$proc_name: Event '$cond_id' - timed out.  This should never happen!"
            }
        }
        ns_mutex unlock $ev_mut

        # We know we are the ONLY worker thread, so we don't have to get too
        # complicated with mutex locking here:
        set queue_l [lsort -dictionary [nsv_array names {ct1_work_queue}]]
        ns_log Notice "$proc_name: [llength $queue_l] work items:  $queue_l"
        foreach ww $queue_l {
            nsv_unset {ct1_work_queue} $ww
            nsv_set {ct1_done} $ww [ns_time]
        }
    }
    ns_log Notice "$proc_name: No more work for me today."
}

proc tst_cond_cleanup {} {
    set aa {cond_test_1}
    set cond_id [nsv_get $aa {cond_id}]
    set ev_mut  [nsv_get $aa {ev_mut}]

    tst_cond_shutdown
    # Wake up the worker thread to tell it we are all done:
    ns_mutex lock $ev_mut
    ns_cond broadcast $cond_id
    ns_mutex unlock $ev_mut

    nsv_unset {ct1_work_queue}
    nsv_unset {ct1_done}
    ns_cond destroy $cond_id

    # We purposely do NOT call "ns_mutex destroy $ev_mut" here.  It is
    # typically a BAD practice to dynamically destroy mutexes, because the
    # POSIX spec and most (maybe all) pthread implementations make it
    # extraordinarily difficult to do correctly.  And if you do it wrong,
    # you can easily crash the server!
}
