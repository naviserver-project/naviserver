# -*- Tcl -*-
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Standalone HTTP regression test; use nslog-slow.nscfg, not the general test configuration.
proc runSlowLogTests {} {
proc assert {condition msg} {if {![uplevel 1 [list expr $condition]]} {error $msg}; puts "PASS: $msg"}
proc hasRequest {log path} {expr {[string first "\"$path\"" $log] >= 0 || [string first "\"GET $path " $log] >= 0}}
proc readlog {name} {set path [file join [ns_config test logdir] $name]; if {![file exists $path]} {return ""}; set f [open $path]; try {read $f} finally {close $f}}
ns_eval -sync {
 proc slowtest {delay} {nsv_set slowtest entered 1; ns_sleep $delay; ns_return 200 text/plain ok}
 proc slowfilter args {ns_sleep 0.08; return filter_ok}
 ns_register_proc GET /fast slowtest 0
 ns_register_proc GET /slow slowtest 0.15
 ns_register_proc GET /filtered slowtest 0
 ns_register_filter preauth GET /filtered slowfilter
}
set url [ns_config test url]
foreach path {/fast /slow /filtered} {ns_http run -timeout 5s $url$path}
nsv_set slowtest entered 0
set first [ns_http queue -timeout 5s $url/slow]
set deadline [expr {[clock milliseconds] + 5000}]
while {![nsv_get slowtest entered]} {
 if {[clock milliseconds] > $deadline} {error {first request did not enter}}
 ns_sleep 0.001
}
set second [ns_http queue -timeout 5s $url/fast]
ns_http wait $first
ns_http wait $second
# Six records also flush the main log when maxbuffer is two.
set headers [ns_set create]
ns_set put $headers X-Slow-Test [string repeat x 9000]
ns_http run -timeout 5s -headers $headers $url/fast?private=hidden
ns_set free $headers
# Wait for asynchronous writes to be visible.
set deadline [expr {[clock milliseconds] + 5000}]
while {[llength [split [string trim [readlog zero-slow.log]] \n]] < 6} {
 if {[clock milliseconds] > $deadline} {error {slow log writes did not finish}}
 ns_sleep 0.01
}
set con [readlog conn-slow.log]
set fil [readlog filter-slow.log]
set que [readlog queue-slow.log]
assert {[hasRequest $con /slow]} {runtime selects slow requests}
assert {[hasRequest $con /filtered]} {connection time includes filter time}
assert {![hasRequest $con /fast]} {fast runtime is not selected}
assert {[hasRequest $fil /filtered] && ![hasRequest $fil /slow]} {filter-only threshold selects independently}
assert {[hasRequest $que /fast]} {queue-only threshold selects a fast queued request}
assert {[readlog none-slow.log] eq ""} {omitted thresholds select nothing}
assert {[readlog zero-access.log] eq [readlog zero-slow.log]} {zero threshold and identical record format}
set either [readlog either-slow.log]
assert {[hasRequest $either /slow] && [hasRequest $either /fast]} {multiple thresholds use OR selection}
assert {![file exists [file join [ns_config test logdir] disabled-slow.log]]} {missing slowlogfile disables the extra log}
assert {[string match {127.0.0.0 *} [readlog zero-slow.log]]} {address masking also applies to the slow log}
assert {![string match *private=hidden* [readlog zero-slow.log]]} {query suppression also applies to the slow log}
assert {[string length [readlog zero-slow.log]] > 9000} {large extended-header records are preserved}
ns_accesslog roll
assert {[file exists [file join [ns_config test logdir] zero-access.log.000]] && [file exists [file join [ns_config test logdir] zero-slow.log.000]]} {main and slow logs roll together}
ns_http run -timeout 5s $url/slow
ns_sleep 0.05
assert {[hasRequest [readlog zero-slow.log] /slow]} {logging continues after rotation}
set renamed [file join [ns_config test logdir] zero-manual.log]
ns_accesslog roll $renamed
assert {[file exists [file join [ns_config test logdir] zero-slow.log.001]]} {explicit-path rotation also rolls the slow log}
set argv {}
source [file join [ns_config test sourceRoot] tests ns_accesslog.test]
puts DONE

}
if {[catch {runSlowLogTests} message options]} {
 puts stderr [dict get $options -errorinfo]
 return -options $options $message
}
