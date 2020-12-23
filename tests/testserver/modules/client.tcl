# -*- Tcl -*-
namespace eval ::tcltest {
    #
    # A simple Tcl client used solely for testing and debugging of
    # buffering in persistent HTTP connections. Since the code uses
    # plain sockets, it is restrictued the plain HTTP requests.

    # The proc tcltest::client receives a number of HTTP requests
    # followed by a list of chunks which form the HTTP requests. In
    # contrary to the classical test client, this procs might send a
    # single request in multiple chunks to the server.
    #
    # The client stops after having received the specified number of
    # replies from the server.
    #
    proc client_log {msg} {
        if {$::tcltest::verbose} {
            puts stderr "### $msg"
        }
    }

    proc client_send {s} {
        set cmd [lindex $::tcltest::cmds 0]
        client_log "send $s <$cmd>"
        set ::tcltest::cmds [lrange $::tcltest::cmds 1 end]
        if {$cmd ne ""} {
            puts -nonewline $s $cmd
            flush $s
            tcltest::client_send $s
        }
    }

    proc client_parserequest {toparse} {
        # A simple minded request HTTP reply parser, needed, since we
        # might receive in on one input multiple replies from the
        # server.
        append ::tcltest::received $toparse
        #client_log "toparse [string length $::tcltest::received]"
        set contentLength 0
        set bytes ""
        set received 0
        set index [string first \n\n $::tcltest::received]
        if {$index > -1} {
            set head [string range $::tcltest::received 0 $index-1]
            set rest [string range $::tcltest::received $index+2 end]
            #client_log "STRING <$::tcltest::received>"
            #client_log "HEAD <$head>"
            #client_log "rest <$rest>"
            regexp {Content-Length:\s+(\d+)\s} $head . contentLength
            if {$contentLength > 0} {
                if {[string length $rest] >= $contentLength} {
                    set bytes $head\n\n[string range $rest 0 $contentLength-1]
                    set toparse [string range $rest $contentLength end]
                } else {
                    #client_log "need more content-length $contentLength <$rest>"
                    set toparse $::tcltest::received
                }
            } else {
                set bytes $head\n\n
                set toparse $rest
            }
            incr received
            set ::tcltest::received $toparse
        }
        set more [expr {$toparse ne ""}]
        client_log "eceived $received bytes [string length $bytes] more $more"

        return [list bytes $bytes more $more received $received]
    }

    proc client_readable {t s} {
        set n [ns_socknread $s]
        if {$n == 0} {
            set ready [ns_sockselect -timeout $t $s {} {}]
            if {[lindex $ready 0] eq {}} {
                error "client_readable: read timed out"
            }
            set n [ns_socknread $s]
        }
        return $n
    }

    proc client_receive {s} {
        client_readable 1000 $s
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
            client_log "received: <$bytes>"

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
        client_log "ns_sockopen $host $port -> rfd $rfd wfd $wfd"

        set sockerr [fconfigure $rfd -error]
        if {$sockerr ne {}} {return -code error $sockerr}
        fconfigure $rfd -translation crlf -blocking 0
        fconfigure $wfd -translation crlf -blocking 1
        fileevent $rfd readable [list tcltest::client_receive $rfd]
        ::tcltest::client_send $wfd
        vwait ::tcltest::forever
        fileevent $rfd readable {}
        client_log "close: $rfd $wfd"

        close $rfd
        close $wfd

        set result $::tcltest::results
        unset -nocomplain ::tcltest::forever ::tcltest::results
        return $result
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
