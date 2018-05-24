namespace eval ::ns_listencallback {
    proc conn_handler {rfd wfd} {
        #ns_log notice "--- conn_handler called with $rfd $wfd"
        puts $wfd "Welcome to the test server"
        flush $wfd
        #ns_log notice "--- conn_handler reads from $rfd"
        gets $rfd line
        #ns_log notice "--- conn_handler got <$line>"
        puts $wfd "Well isn't that nice"
        flush $wfd
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
