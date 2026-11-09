# Child-process probe used by nscp.test. Requires only an installed binary/module.
proc probeRead {channel suffix} {
    set deadline [expr {[clock milliseconds]+5000}]
    set data {}
    while {[clock milliseconds]<$deadline} {
        append data [ns_connchan read $channel]
        if {[string match *$suffix $data]} {return $data}
        ns_sleep 0.005
    }
    error "nscp listener did not return expected prompt"
}
proc probeWrite {channel data} {
    set deadline [expr {[clock milliseconds]+5000}]
    ns_connchan write $channel $data
    while {[dict get [ns_connchan status $channel] sendbuffer]>0} {
        if {[clock milliseconds]>$deadline} {error "nscp write timed out"}
        ns_sleep 0.005
        ns_connchan write $channel ""
    }
}

set listeners {}
set deadline [expr {[clock milliseconds]+2000}]
while {[clock milliseconds]<$deadline} {
    set listeners {}
    foreach callback [ns_info sockcallbacks] {
        if {[lindex $callback 2] eq "nscp"} {lappend listeners [lindex $callback 3]}
    }
    if {[llength $listeners]>0} {break}
    ns_sleep 0.01
}
set expected [ns_config nscp-test expected]
set actual {};set ports {}
foreach listener $listeners {lappend actual [lindex $listener 0];lappend ports [lindex $listener 1]}
set actual [lsort $actual]
# ns_addrbyhost intentionally applies outbound AI_ADDRCONFIG, unlike listener
# resolution. Do not use it as the exact expected localhost address set.
if {$expected eq "@localhost"} {
    if {[llength $actual]==0} {error "localhost did not produce a listener"}
    set expected $actual
}
if {$actual ne [lsort $expected]} {error "expected listeners $expected, observed $actual"}
set ports [lsort -unique $ports]
if {[llength $ports]!=1 || [lindex $ports 0]<=0} {error "listeners do not share one valid port: $listeners"}
set local [ns_connchan connect -timeout 5s localhost [lindex $ports 0]]
ns_connchan close $local
set results {}
foreach address $expected {
    set channel [ns_connchan connect -timeout 5s $address [lindex $ports 0]]
    ns_connchan callback -receivetimeout 5s -sendtimeout 5s \
        $channel {apply {{reason} {return 0}}} x
    set code [catch {
        probeRead $channel {login: }
        probeWrite $channel "\r\n"
        probeRead $channel {Password: }
        probeWrite $channel "\r\n"
        probeRead $channel {test:nscp 1> }
        probeWrite $channel "list \[expr {40+2}\] \[nscp users\]\r\n"
        set reply [probeRead $channel {test:nscp 2> }]
        if {[string first "42 {{} nsadmin}\r\n" $reply]<0} {error "nscp evaluation/users failed: $reply"}
        lappend results $address
    } message options]
    ns_connchan close $channel
    if {$code} {return -options $options $message}
}
set f [open [ns_config nscp-test result] w]
puts $f [dict create addresses [lsort $results] listenerCount [llength $listeners] ports [llength $ports]]
close $f
ns_shutdown
vwait forever
