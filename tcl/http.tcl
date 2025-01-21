#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
#

# http.tcl
#
#   Routines for making nonblocking HTTP connections through
#   the ns_socket interface.
#

#
# Simple http proxy handler.
# All requests that start with http:// will be proxied.
#

if {[ns_config -bool -set ns/server/[ns_info server] enablehttpproxy off]} {

    foreach proto {http https} {
        foreach httpMethod {GET POST} {
            ns_register_proxy $httpMethod $proto ns_proxy_request
        }
        ns_register_proxy CONNECT "" ns_proxy_connect
    }
    nsv_set ns:proxy allow [ns_config -set ns/server/[ns_info server] allowhttpproxy]

    proc ns_proxy_request { args } {
        ns_log warning "======== ns_proxy_request is called" args <$args> (server [ns_info server])
        #
        # Get the full URL from request line
        #
        if {![regexp {^\S+\s(\S.+)\s\S+$} [ns_conn request] . URL]} {
            ns_log warning "proxy: request line malformed: <[ns_conn request]>"
            ns_return 400 text/plain "invalid proxy request"
        } elseif {[info commands ::revproxy::ns_http::upstream] ne ""} {
            ns_log notice ::revproxy::ns_http::upstream -url $URL
            return [::revproxy::ns_http::upstream \
                        -url $URL \
                        -request [::revproxy::request] \
                        -spoolresponse true \
                       ]
        } else {
            # Simple fallback handler
            ns_log warning ns_http run -method [ns_conn method] -spoolsize 0 $URL
            set d [ns_http run -method [ns_conn method] -spoolsize 0 $URL]
            #ns_log notice ... $d
            ns_headers [dict get $d status] [ns_set get -nocase [dict get $d headers] content-type]
            ns_writer submitfile [dict get $d file]
            file delete [dict get $d file]
        }
    }
    proc ns_proxy_connect { args } {
        ns_log warning "======== ns_proxy_connect is called" args <$args> (server [ns_info server])
        ns_log notice [ns_set format [ns_conn headers]]
        set peeraddr   [ns_conn peeraddr]
        #
        # We could/should add Proxy-Authorization here.  For now, we
        # reject all requests from public clients and trust internal
        # ones.
        #
        if { [ns_ip public $peeraddr]} {
            ns_return 405 text/plain "CONNECT Request Rejected"
        } else {
            try {
                set targetHost [ns_conn host]
                set targetPort [ns_conn port]
                set frontendChan [ns_connchan detach]
                set backendChan [ns_connchan connect -timeout 1s $targetHost $targetPort]
                ns_connchan write $frontendChan "HTTP/1.1 200 Connection Established\r\n\r\n"

                set identifier "CONNECT ${targetHost} ${targetPort}"
                ns_log notice $identifier frontendChan $frontendChan backendChan $backendChan \
                    peeraddr $peeraddr public [ns_ip public $peeraddr] trusted [ns_ip trusted $peeraddr]
                set timeouts {-sendtimeout 1s -receivetimeout 1s -timeout 1m}
                set timeoutArgs [list -timeout [dict get $timeouts -timeout] \
                                     -sendtimeout [dict get $timeouts -sendtimeout] \
                                     -receivetimeout [dict get $timeouts -receivetimeout]]
                ns_connchan callback {*}$timeoutArgs \
                    $frontendChan [list ::revproxy::ns_connchan::spool $frontendChan $backendChan $identifier $timeouts ""] rex
                ns_connchan callback {*}$timeoutArgs \
                    $backendChan  [list ::revproxy::ns_connchan::spool $backendChan $frontendChan $identifier $timeouts ""] rex
            } on error {errorMsg} {
                ns_log error ns_proxy_connect: $errorMsg
                catch {ns_connchan write $frontendChan "HTTP/1.0 500 Interal server error\r\n\r\n"}
            }
        }
    }

}

#######################################################################################
# Deprecated procs
#######################################################################################

if {[dict get [ns_info buildinfo] with_deprecated]} {
    #
    # ns_httpopen --
    #
    #   Opens connection to remote client using http protocol.
    #
    # Results:
    #   Tcl list {read channel} {write channel} {result headers set}
    #   Channels are left in blocking mode and binary translation.
    #
    # Side effects:
    #   None.
    #

    proc ns_httpopen {method url {rqset ""} {timeout 30} {pdata ""}} {

        ns_deprecated "ns_http"
        #
        # Determine if URL is local; prepend site address if yes
        #

        if {[string match "/*" $url]} {
            set conf ns/server/[ns_info server]/module/nssock
            set host "http://[ns_config $conf hostname]"
            set port [ns_config $conf port]
            if {$port != 80} {
                append host ":$port"
            }
            set url "$host$url"
        }

        #
        # Verify that the URL is an HTTP URL.
        #

        if {![string match "http://*" $url]} {
            error "Invalid URL \"$url\": " "ns_httpopen only supports HTTP"
        }

        #
        # Find each element in the URL
        #

        set url  [split $url /]
        set hp   [split [lindex $url 2] :]
        set host [lindex $hp 0]
        set port [lindex $hp 1]

        if {$port eq {}} {
            set port 80
        }

        set uri /[join [lrange $url 3 end] /]

        #
        # Open a TCP connection to the host:port
        #

        set fds [ns_sockopen -nonblock $host $port]
        set rfd [lindex $fds 0]
        set wfd [lindex $fds 1]

        set sockerr [fconfigure $rfd -error]
        if {$sockerr ne {}} {
            return -code error $sockerr
        }

        fconfigure $rfd -translation auto -encoding utf-8 -blocking 0
        fconfigure $wfd -translation crlf -encoding utf-8 -blocking 1

        if {[catch {

            #
            # First write the request, then the headers.
            #

            _ns_http_puts $timeout $wfd "$method $uri HTTP/1.0"

            if {$rqset ne {}} {

                #
                # There are request headers to pass.
                #

                for {set i 0} {$i < [ns_set size $rqset]} {incr i} {
                    set key [ns_set key   $rqset $i]
                    set val [ns_set value $rqset $i]
                    _ns_http_puts $timeout $wfd "$key: $val"
                }

                if {$pdata ne {}} {
                    set pdlen [ns_set iget $rqset "content-length"]
                    if {$pdlen eq {}} {
                        set pdlen [string length $pdata]
                        _ns_http_puts $timeout $wfd "content-length: $pdlen"
                    }
                }

            } else {

                #
                # No headers were specified, so send a minimum set
                # of required headers.
                #

                _ns_http_puts $timeout $wfd "accept: */*"
                _ns_http_puts $timeout $wfd \
                    "user-agent: [ns_info name]-Tcl/[ns_info version]"

                if {$pdata ne {}} {
                    set pdlen [string length $pdata]
                    _ns_http_puts $timeout $wfd "content-length: $pdlen"
                }
            }

            #
            # Always send a Host: header because virtual hosting happens
            # even with HTTP/1.0.
            #

            if {$rqset eq "" || [ns_set iget $rqset Host] eq ""} {
                if {$port == 80} {
                    _ns_http_puts $timeout $wfd "Host: $host"
                } else {
                    _ns_http_puts $timeout $wfd "Host: $host:$port"
                }
            }

            #
            # If optional content exists, send it.
            #

            _ns_http_puts $timeout $wfd ""
            flush $wfd

            if {$pdata ne {}} {
                fconfigure $wfd -translation binary -blocking 1
                _ns_http_write $timeout $wfd $pdata $pdlen
            }

            #
            # Create a new set; its name will be the result line from
            # the server. Then read headers into the set.
            #

            set rpset [ns_set new [_ns_http_gets $timeout $rfd]]
            while {1} {
                set line [_ns_http_gets $timeout $rfd]
                if {$line eq ""} {
                    break ; # Empty line - end of headers
                }
                ns_parseheader $rpset $line
            }

        } err]} {

            #
            # Something went wrong during the request, so return an error.
            #

            catch {close $wfd}
            catch {close $rfd}

            if {[info exists rpset]} {
                ns_set free $rpset
            }

            return -code error -errorinfo $::errorInfo $err
        }

        #
        # Return a list of read, write channel
        # and headers from remote.
        #

        foreach fd [list $rfd $wfd] {
            fconfigure $fd -translation binary -blocking 1
        }

        return [list $rfd $wfd $rpset]
    }

    #
    # ns_httppost --
    #
    #   Perform a POST request. This wraps ns_httpopen.
    #
    # Results:
    #   The URL content.
    #
    # Side effects:
    #   None.
    #

    proc ns_httppost {url {rqset ""} {qsset ""} {type ""} {timeout 30}} {

        ns_deprecated "ns_http"

        #
        # Build the request. Since we're posting, we have to set
        # content-type and content-length ourselves. We'll add
        # these to rqset, overwriting ones already in the set.
        #

        if {$rqset eq {}} {

            #
            # Generate minimum set of headers for remote
            #

            set rqset [ns_set new rqset]
            ns_set put $rqset "accept" "*/*"
            ns_set put $rqset "user-agent" "[ns_info name]-Tcl/[ns_info version]"
        }

        if {$type eq {}} {
            set type "application/x-www-form-urlencoded"
        }

        ns_set put $rqset "content-type" "$type"

        #
        # Build the query string to POST with.
        #

        set pdata {}

        if {$qsset eq {}} {
            ns_set put $rqset "content-length" "0"
        } else {
            for {set i 0} {$i < [ns_set size $qsset]} {incr i} {
                set key [ns_set key   $qsset $i]
                set val [ns_set value $qsset $i]
                if {$i > 0} {
                    append pdata "&"
                }
                append pdata $key "=" [ns_urlencode $val]
            }
            ns_set put $rqset "content-length" [string length $pdata]
        }

        #
        # Perform the actual request.
        #

        set fds [ns_httpopen POST $url $rqset $timeout $pdata]
        set rfd [lindex $fds 0]
        set wfd [lindex $fds 1]; close $wfd

        set headers [lindex $fds 2]
        set length  [ns_set iget $headers "content-length"]
        ns_set free $headers

        if {$length eq {}} {
            set length -1
        }

        set err [catch {_ns_http_getcontent $timeout $rfd $length} content]
        close $rfd

        if {$err} {
            return -code error -errorinfo $::errorInfo $content
        }

        return $content
    }

    #
    # ns_httpget --
    #
    #   Perform a GET request. This wraps ns_httpopen, but it also
    #   knows how to follow redirects and will read the content into
    #   a buffer.
    #
    # Results:
    #   The URL content.
    #
    # Side effects:
    #   Will only follow redirections up to 10 levels deep.
    #

    proc ns_httpget {url {timeout 30} {depth 0} {rqset ""}} {

        ns_deprecated "ns_http"

        if {[incr depth] > 10} {
            return -code error "ns_httpget: recursive redirection: $url"
        }

        #
        # Perform the actual request.
        #

        set fds [ns_httpopen GET $url $rqset $timeout]
        set rfd [lindex $fds 0]
        set wfd [lindex $fds 1]; close $wfd

        set headers  [lindex $fds 2]
        set response [ns_set name $headers]
        set status   [lindex $response 1]

        if {$status == 302} {

            #
            # The response was a redirect, recurse.
            #

            set location [ns_set iget $headers "location"]
            if {$location ne {}} {
                ns_set free $headers
                close $rfd
                if {[string first http:// $location] != 0} {
                    set url2 [split $url /]
                    set hp   [split [lindex $url2 2] ":"]
                    set host [lindex $hp 0]
                    set port [lindex $hp 1]
                    if {[string match $port ""]} {
                        set port 80
                    }
                    regexp "^(.*)://" $url match proto
                    set location "$proto://$host:$port/$location"
                }
                return [ns_httpget $location $timeout $depth $rqset]
            }
        }

        set length [ns_set iget $headers "content-length"]
        set encoding [ns_set iget $headers "transfer-encoding"]
        ns_set free $headers

        if {$length eq {}} {
            set length -1
        }

        set err [catch {_ns_http_getcontent $timeout $rfd $length} content]
        close $rfd

        if {$err} {
            return -code error -errorinfo $::errorInfo $content
        }

        #
        # Try to parse chunked encoding and concatenate all
        # chunks into one body
        #

        if {$encoding eq "chunked"} {
            set text ""
            set offset 0
            while {1} {
                # Parse size header
                set end [string first "\n" $content $offset]
                if {$end == -1} {
                    break
                }
                set size [scan [string range $content $offset $end] %x]
                if {$size == 0 || $size eq ""} {
                    break
                }
                set offset [incr end]
                # Read data
                append text [string range $content $offset [expr {$offset+$size-1}]]
                incr offset [incr size]
            }
            set content $text
        }

        return $content
    }


    #
    # _ns_http_readable --
    #
    #   Return the number of bytes available to read from socket
    #   without blocking. Waits up to $timeout seconds for bytes
    #   to arrive. Trigger error on timeout. On EOF returns zero.
    #
    # Results:
    #   Number of bytes waiting to be read
    #
    # Side effects:
    #   None.
    #

    proc _ns_http_readable {timeout sock} {
        ns_deprecated "" "helper of deprecated proc"

        set nread [ns_socknread $sock]
        if {$nread == 0} {
            set ready [ns_sockselect -timeout $timeout $sock {} {}]
            if {[lindex $ready 0] eq {}} {
                return -code error "_ns_http_readable: read timed out"
            }
            set nread [ns_socknread $sock]
        }

        return $nread
    }


    #
    # _ns_http_read --
    #
    #   Read up to $length bytes from a socket without blocking.
    #
    # Results:
    #   Up to $length bytes that were read from the socket.
    #   or zero bytes on EOF. Throws error on timeout.
    #
    # Side effects:
    #   None.
    #

    proc _ns_http_read {timeout sock length} {
        ns_deprecated "" "helper of deprecated proc"

        set nread [_ns_http_readable $timeout $sock]
        if {$length > 0 && $length < $nread} {
            set nread $length
        }

        read $sock $nread
    }


    #
    # _ns_http_gets -
    #
    #   Read a line from socket w/o blocking.
    #
    # Results:
    #   A line read from the socket or error on EOF or timeout.
    #
    # Side effects:
    #   None.
    #

    proc _ns_http_gets {timeout sock} {
        ns_deprecated "" "helper of deprecated proc"

        while {[gets $sock line] == -1} {
            if {[eof $sock]} {
                return -code error "_ns_http_gets: premature end of data"
            }
            _ns_http_readable $timeout $sock
        }

        return $line
    }


    #
    # _ns_http_puts --
    #
    #   Send a string to socket. If the socket buffer is
    #   full, wait for up to $timeout seconds.
    #
    # Results:
    #   None.
    #
    # Side effects:
    #   None.
    #

    proc _ns_http_puts {timeout sock string} {
        ns_deprecated "" "helper of deprecated proc"

        set ready [ns_sockselect -timeout $timeout {} $sock {}]
        if {[lindex $ready 1] eq {}} {
            return -code error "_ns_http_puts: write timed out"
        }

        puts $sock $string
    }

    #
    # _ns_http_write --
    #
    #   Send a string to socket. If the buffer is
    #   full, wait for up to $timeout seconds.
    #
    # Results:
    #   None.
    #
    # Side effects:
    #   None.
    #

    proc _ns_http_write {timeout sock string {length -1}} {
        ns_deprecated "" "helper of deprecated proc"

        set ready [ns_sockselect -timeout $timeout {} $sock {}]
        if {[lindex $ready 1] eq {}} {
            return -code error "_ns_http_puts: write timed out"
        }

        puts -nonewline $sock $string; flush $sock

        return

        #
        # Experimental/debugging block-wise write
        #

        if {$length == -1} {
            set length [string length $string]
        }

        set len [fconfigure $sock -buffersize]
        set beg 0
        set end [expr {$len - 1}]

        while {$beg < $length} {
            if {$end >= $length} {
                set end [expr {$length - 1}]
            }
            set ready [ns_sockselect -timeout $timeout {} $sock {}]
            if {[lindex $ready 1] eq {}} {
                return -code error "_ns_http_puts: write timed out"
            }
            puts -nonewline $sock [string range $string $beg $end]
            incr beg $len
            incr end $len
        }

        flush $sock
    }

    #
    # _ns_http_getcontent --
    #
    #   Slurps entire content from socket into memory.
    #
    # Results:
    #   Content read from socket.
    #
    # Side effects:
    #   Sends received content buffer-wise to currently opened
    #   connection (if any) if the optional arg "copy" is true.
    #

    proc _ns_http_getcontent {timeout sock length {copy 0}} {
        ns_deprecated "" "helper of deprecated proc"

        set content ""

        while {1} {
            set buf [_ns_http_read $timeout $sock $length]
            set len [string length $buf]
            if {$len == 0} {
                break
            }
            if {$copy} {
                ns_write $buf
            } else {
                append content $buf
            }
            if {$length >= 0} {
                set length [expr {$length - $len}]
                if {$length <= 0} {
                    break
                }
            }
        }

        return $content
    }
    proc ns_ssl {args} {

        ns_deprecated "ns_http"
        uplevel [list ns_http {*}$args]
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
