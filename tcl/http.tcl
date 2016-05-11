#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://www.mozilla.org/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is AOLserver Code and related documentation
# distributed by AOL.
# 
# The Initial Developer of the Original Code is America Online,
# Inc. Portions created by AOL are Copyright (C) 1999 America Online,
# Inc. All Rights Reserved.
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
#

# http.tcl
#
#   Routines for making non-blocking HTTP connections through
#   the ns_socket interface.
#

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
    # Determine if url is local; prepend site address if yes
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
    # Verify that the URL is an HTTP url.
    #

    if {![string match "http://*" $url]} {
        error "Invalid url \"$url\": " "ns_httpopen only supports HTTP"
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
                set pdlen [ns_set iget $rqset "Content-Length"]
                if {$pdlen eq {}} {
                    set pdlen [string length $pdata]
                    _ns_http_puts $timeout $wfd "Content-Length: $pdlen"
                }
            }

        } else {

            #
            # No headers were specified, so send a minimum set
            # of required headers.
            #

            _ns_http_puts $timeout $wfd "Accept: */*"
            _ns_http_puts $timeout $wfd \
                "User-Agent: [ns_info name]-Tcl/[ns_info version]"
            
            if {$pdata ne {}} {
                set pdlen [string length $pdata]
                _ns_http_puts $timeout $wfd "Content-Length: $pdlen"
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
        ns_set put $rqset "Accept" "*/*"
        ns_set put $rqset "User-Agent" "[ns_info name]-Tcl/[ns_info version]"
    }

    if {$type eq {}} {
        set type "application/x-www-form-urlencoded"
    }

    ns_set put $rqset "Content-type" "$type"

    #
    # Build the query string to POST with.
    #

    set pdata {}

    if {$qsset eq {}} {
        ns_set put $rqset "Content-length" "0"
    } else {
        for {set i 0} {$i < [ns_set size $qsset]} {incr i} {
            set key [ns_set key   $qsset $i]
            set val [ns_set value $qsset $i]
            if {$i > 0} {
                append pdata "&"
            }
            append pdata $key "=" [ns_urlencode $val]
        }
        ns_set put $rqset "Content-length" [string length $pdata]
    }

    #
    # Perform the actual request.
    #

    set fds [ns_httpopen POST $url $rqset $timeout $pdata]
    set rfd [lindex $fds 0]
    set wfd [lindex $fds 1]; close $wfd

    set headers [lindex $fds 2]
    set length  [ns_set iget $headers "Content-Length"]
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

        set location [ns_set iget $headers "Location"]
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

    set length [ns_set iget $headers "Content-Length"]
    set encoding [ns_set iget $headers "Transfer-Encoding"]
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

#
# Simple http proxy handler.
# All requests that start with http:// will be proxied.
#

if {[ns_config -bool -set ns/server/[ns_info server] enablehttpproxy off]} {
    ns_register_proxy GET  http ns_proxy_handler_http
    ns_register_proxy POST http ns_proxy_handler_http
    nsv_set ns:proxy allow [ns_config -set ns/server/[ns_info server] allowhttpproxy]
}

proc ns_proxy_handler_http {args} {
    
    set allow 0
    set peeraddr [ns_conn peeraddr]
    foreach ip [nsv_get ns:proxy allow] {
      if { [string match $ip $peeraddr] } {
        set allow 1
        break
      }
    }
    if { $allow != 1 } {
      ns_log Error ns_proxy_handler_http: access denied for $peeraddr
      ns_returnnotfound
      return
    }

    set port [ns_conn port]
    if {$port == 0} {
        set port 80
    }
    set cont   [ns_conn content]
    set prot   [ns_conn protocol]
    set method [ns_conn method]

    set url $prot://[ns_conn host]:$port[ns_conn url]?[ns_conn query]

    set fds [ns_httpopen $method $url [ns_conn headers] 30 $cont]
    set rfd [lindex $fds 0]
    set wfd [lindex $fds 1]; close $wfd

    set headers  [lindex $fds 2]
    set response [ns_set name $headers]
    set status   [lindex $response 1]
    set length   [ns_set iget $headers "Content-Length"]
    if {$length eq {}} {
        set length -1
    }

    if {[catch {
        ns_write "$response\r\n"
        for {set i 0} {$i < [ns_set size $headers]} {incr i} {
            set key [ns_set key   $headers $i]
            set val [ns_set value $headers $i]
            ns_write "$key: $val\r\n"
        }
        ns_write "\r\n"
        _ns_http_getcontent 30 $rfd $length 1
    } err]} {
        ns_log error $err
    }

    ns_set free $headers
    close $rfd
}

proc ns_ssl {args} {

    ns_deprecated "ns_http"
    uplevel [list ns_http {*}$args]
}


# EOF

