<%
# Issue an HTTP request to a nonexistent address
if {0} {
    ns_log notice "REQUEST START"
    set r [ns_http run http://192.0.2.1/]
    ns_log notice "REQUEST DONE $r"
    ns_adp_puts $r
    set r
} {
    if {$::tcl_version < 8.6} {package require try}
    try {
        ns_log notice "REQUEST START"
        set r [ns_http run http://192.0.2.1/]
        ns_log notice "REQUEST DONE $r"
        ns_adp_puts $r
        set r
    } trap NS_TIMEOUT {r} {
        #puts stderr "REQUEST ends in Timeout"
        ns_log notice "Trap NS_TIMEOUT $r"
    } on error {errorMsg} {
        puts stderr "REQUEST ends in ERROR: $errorMsg"
        #
        # The behavior on Linux and macOS is different. While an
        # attempt to connect to a non-existent IP address under macOS
        # leads for async connections to a timeout, we see an error
        # under Linux (socket is not connected). Make sure, we trigger
        # a timeout as well.
        #
        ns_log error "connect to a nonexistent address: $::errorCode, errorMsg $errorMsg"
        ns_sleep 5s
    } on ok {r} {
        ns_log notice "REQUEST ends OK"
    }
}
%>
