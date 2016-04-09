# -*- Tcl -*-
namespace eval ::tcltest {
    #
    # A simple tcl client used for testing and debugging of buffering
    # in persistent HTTP connections. The proc tcltest::client
    # receives a number of HTTP requests followed by a list of chunks
    # which form the HTTP requests. In contrary to the classical test
    # client, this procs might send a single request in multiple
    # chunks to the server.
    #
    # The client stops, after heaving received the specified number of
    # replies from the server.
    #
    proc client_send {s} {
	set cmd [lindex $::tcltest::cmds 0]
	if {$::tcltest::verbose} {puts stderr "### send $s <$cmd>"}
	set ::tcltest::cmds [lrange $::tcltest::cmds 1 end]
	if {$cmd ne ""} {
	    puts -nonewline $s $cmd
	    flush $s
	    tcltest::client_send $s
	}
    }
    
    proc client_parserequest {toparse} {
	# A simple minded request http reply parser, needed, since we
	# might receive in on one input multiple replies from the
	# server.
	append ::tcltest::received $toparse
	#puts stderr "### toparse [string length $::tcltest::received]"
	set contentLength 0
	set bytes ""
	set received 0
	set index [string first \n\n $::tcltest::received]
	if {$index > -1} {
	    set head [string range $::tcltest::received 0 $index-1]
	    set rest [string range $::tcltest::received $index+2 end]
	    #puts stderr "STRING <$::tcltest::received>"
	    #puts stderr "### HEAD <$head>"
	    #puts stderr "### rest <$rest>"
	    regexp {Content-Length:\s+(\d+)\s} $head . contentLength
	    if {$contentLength > 0} {
		set bytes $head\n\n[string range $rest 0 $contentLength-1]
		set toparse [string range $rest $contentLength end]
	    } else {
		set bytes $head\n\n
		set toparse $rest
	    }
	    incr received
	    set ::tcltest::received $toparse
	}
	set more [expr {[string length $toparse] > 0}]
	if {$::tcltest::verbose} {puts stderr "### received $received bytes [string length $bytes] more $more"}
	return [list bytes $bytes more $more received $received]
    }
    
    proc client_receive {s} {
	set input [read $s]
	if {$input eq ""} {
	    set ::tcltest::forever 0
	    return
	}
	while {1} {
	    set d [client_parserequest $input]
	    set input ""
	    set bytes [dict get $d bytes]
	    set more  [dict get $d more]
	    set size  [string length $bytes]
	    if {$size == 0} break
	    dict set ::tcltest::results $::tcltest::nrCmds size $size
	    dict set ::tcltest::results $::tcltest::nrCmds bytes $bytes
	    if {$::tcltest::verbose} {puts stderr "### received: <$bytes>"}
	    if {[incr ::tcltest::nrCmds -1] < 1} {
		set ::tcltest::forever 0
		break
	    }
	    if {$more == 0} {
		break
	    }
	}
    }
    
    proc client {nrCmds cmds {verbose 0}} {
	set ::tcltest::cmds $cmds
	set ::tcltest::nrCmds $nrCmds
	set ::tcltest::verbose $verbose
	
	set host [ns_config "test" loopback]
	set port [ns_config "test" listenport]
	lassign [ns_sockopen $host $port] rfd wfd
	if {$::tcltest::verbose} {puts stderr "### ns_sockopen $host $port -> rfd $rfd wfd $wfd"}
	set sockerr [fconfigure $rfd -error]
	if {$sockerr ne {}} {return -code error $sockerr}
	fconfigure $rfd -translation crlf -blocking 0
	fconfigure $wfd -translation crlf -blocking 1
	fileevent $rfd readable [list tcltest::client_receive $rfd]
	::tcltest::client_send $wfd
	vwait ::tcltest::forever
	fileevent $rfd readable {}
	if {$::tcltest::verbose} {puts stderr "### close: $rfd $wfd"}
	close $rfd
	close $wfd

	set result $::tcltest::results
	unset -nocomplain ::tcltest::forever ::tcltest::results
	return $result
    }
}
