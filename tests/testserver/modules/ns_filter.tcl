# -*- Tcl -*-

# These two files work together for testing ns_cond:
#   tests/testserver/modules/nscond.tcl
#   tests/ns_cond.test

proc ::_ns_filter_test {what result args} {
    #ns_log notice "CALLED ::_ns_filter_test $what $result <$args>"
    if {[llength $args] ne 0} {
	set script [lindex $args 0]
	eval $script
    }
    return $result
}


# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
