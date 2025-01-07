#-------------------------------------------------------------------------------
# Implementation of the reverse proxy backend connection via "ns_connchan"
#-------------------------------------------------------------------------------

namespace eval ::revproxy {}
namespace eval ::revproxy::ns_connchan {

    #
    # Upstream handler (deliver request from an upstream server)
    #
    # Serve the requested file from an upstream server, which might be
    # an HTTP or HTTPS server. NaviServer acts as a reverse proxy
    # server.  The upstream function works incrementally and functions
    # as well for WebSockets (including secure WebSockets).  Note that
    # we can specify for every filter registration different parameters
    # (e.g. different timeouts).
    #

    nsf::proc upstream {
        -url
        {-timeout 10.0s}
        {-sendtimeout 0.5s}
        {-receivetimeout 0.5s}
        {-validation_callback ""}
        {-regsubs:0..n ""}
        {-exception_callback "::revproxy::exception"}
        {-url_rewrite_callback "::revproxy::rewrite_url"}
        {-backend_response_callback ""}
        -request:required
        {-spoolresponse true}
    } {
        #
        # @param spoolresponse dummy parameter just for interface compatibility
        #        with ns_http variant
        #

        set requestHeaders [dict get $request headers]
        set method         [dict get $request method]
        #
        # Inject a "connection close" instruction to avoid persistent
        # connections to the backend. Otherwise we would not be able
        # to use the registration url (the url passed to
        # ns_register_filter) for rewriting incoming requests, since
        # the client sends for persistent connections the request
        # unmodified sent via the already open channel.
        #
        # We might have to take more precautions for WebSockets here.
        #
        ns_set iupdate $requestHeaders connection close
        #ns_log notice requestHeaders=[ns_set array $requestHeaders]

        if {$validation_callback ne ""} {
            try {
                set request [{*}$validation_callback -url $url -request $request]
                #
                # When the callback returns the empty string, request
                # processing ends here. In this case the callback is
                # responsible for sending a response to the client.
                #
                if {$request eq ""} {
                    return filter_return
                }
            } on error {errorMsg} {
                #
                # Try old-style invocation (cannot modify request data)
                #
                ns_log warning "revproxy: validation callback failed. Try old-style invocation"

                {*}$validation_callback -url $url
            }
        }

        #
        # Support for Unix Domain Sockets
        # Syntax: unix:/home/www.socket|http://localhost/whatever/
        # modeled after: https://httpd.apache.org/docs/trunk/mod/mod_proxy.html#proxypass

        if {[regexp {^unix:(/[^|]+)[|](.+)$} $url . socketPath url]} {
            set unixSocketArg [list -unix_socket $socketPath]
        } else {
            set unixSocketArg ""
        }

        if {[catch {
            #
            # Open backend channel, get frontend channel and connect these.
            #
            set backendChan [ns_connchan open \
                                 {*}$unixSocketArg \
                                 -method $method \
                                 -headers $requestHeaders \
                                 -timeout $timeout \
                                 -version [dict get $request version] \
                                 $url]
            #
            # Have with gotten the body as a string or file?
            #
            set chunk 16000
            if {[dict exists $request contentfile]} {
                #
                # file content
                #
                set F [open [dict get $request contentfile] r]
                fconfigure $F -translation binary
                while {1} {
                    log notice "upstream: send max $chunk bytes from file to $backendChan " \
                        "(length $contentLength)"
                    ns_connchan write -buffered $backendChan [read $F $chunk]
                    if {[eof $F]} break
                }
                close $F
            } else {
                #
                # string content
                #
                set data [dict get $request content]
                set length [string length $data]
                set i 0
                set j [expr {$chunk -1}]
                while {$i < $length} {
                    log notice "upstream: send max $chunk bytes from string to $backendChan " \
                        "(length $contentLength)"
                    ns_connchan write -buffered $backendChan [string range $data $i $j]
                    incr i $chunk
                    incr j $chunk
                }
            }

            #
            # Check status of backend channel. In particular, we check
            # whether some content is still buffered in the channel.
            #
            # We could send the data here as well with a loop, since
            # we are here still in the connection thread. Nowever, it
            # is more efficient to trigger I/O operation via the
            # writable condition in the socks thread, since this will
            # not block the connection thread.
            #
            set frontendChan [ns_connchan detach]
            log notice "backendChan $backendChan frontendChan $frontendChan " \
                "method $method version 1.0 $url"
            set done_cmd [list ::revproxy::ns_connchan::upstream_send_done \
                              -backendChan $backendChan \
                              -frontendChan $frontendChan \
                              -url $url \
                              -timeout $timeout \
                              -sendtimeout $sendtimeout \
                              -receivetimeout $receivetimeout \
                              -backend_response_callback $backend_response_callback \
                              -exception_callback $exception_callback]

            set status [ns_connchan status $backendChan]
            if {[dict get $status sendbuffer] == 0} {
                {*}$done_cmd
            } else {
                ns_log notice "revproxy::upstream: sending request to the client is not finished yet: $status"
                ns_connchan callback $backendChan \
                    [list revproxy::upstream_send_writable $backendChan $done_cmd] wex
            }
        } errorMsg]} {
            ::revproxy::ns_connchan::upstream_send_failed \
                -errorMsg $errorMsg \
                -url $url \
                -exception_callback $exception_callback \
                {*}[expr {[info exists frontendChan] ? "-frontendChan $frontendChan" : ""}] \
                {*}[expr {[info exists backendChan]  ? "-backendChan $backendChan" : ""}]
        }
        return filter_return
    }

    nsf::proc upstream_send_writable {
        channel
        done_cmd
        condition
    } {
        # When continue is 1, the event will fire again; when continue
        # is 0 channel will be closed.  A continue of 2 means cancel
        # the callback, but don't close the channel.
        #
        log notice "upstream_send_writable on $channel (condition $condition)"

        set result [ns_connchan write -buffered $channel ""]
        set status [ns_connchan status $channel]
        #ns_log notice "upstream_send writable <$result> status $status"
        if {$result == 0 || [dict get $status sendbuffer] > 0} {
            #ns_log warning "upstream_send_writable still flushing the buffer " \
                "(still [dict get $status sendbuffer])... trigger again. status: $status"
            set continue 1
        } else {
            #
            # All was sent,
            #
            set continue 1
            {*}$done_cmd
            ns_log notice "revproxy::upstream_send_writable all was sent, register callback for reading DONE"
        }

        log notice "upstream_send_writable returns $continue (channel $channel)"
        return $continue
    }

    nsf::proc upstream_send_done {
        -backendChan
        -frontendChan
        -url
        -timeout
        -sendtimeout
        -receivetimeout
        -backend_response_callback
        -exception_callback
    } {
        #
        # Full request was received and transmitted upstream, now
        # setup for replies.
        #
        #ns_log notice "=== upstream_send_done ==="
        try {
            set timeouts [list -timeout $timeout -sendtimeout $sendtimeout -receivetimeout $receivetimeout]
            #log notice "===== Set callbacks [ns_info server] frontendChan $frontendChan backendChan $backendChan"
            #
            # We had before instead of $url in the "spool"
            # registration the constant client. However, for the end
            # user messages, it is more friendly to provide the
            # upstream URL and not necessarily the target URL.
            #
            ns_connchan callback \
                -timeout $timeout -sendtimeout $sendtimeout -receivetimeout $receivetimeout \
                $frontendChan [list ::revproxy::ns_connchan::spool $frontendChan $backendChan $url $timeouts ""] rex
            ns_connchan callback \
                -timeout $timeout -sendtimeout $sendtimeout -receivetimeout $receivetimeout \
                $backendChan [list ::revproxy::ns_connchan::backendResponse -callback $backend_response_callback \
                                  $backendChan $frontendChan $url $timeouts ""] rex

        } on error {errorMsg} {
            ns_log error "upstream_send_done: $errorMsg"
            ::revproxy::ns_connchan::upstream_send_failed \
                -errorMsg $errorMsg \
                -url $url \
                -exception_callback $exception_callback \
                {*}[expr {[info exists frontendChan] ? "-frontendChan $frontendChan" : ""}] \
                {*}[expr {[info exists backendChan]  ? "-backendChan $backendChan" : ""}]
        }
    }

    nsf::proc upstream_send_failed {
        -errorMsg
        -frontendChan
        -backendChan
        -url
        {-exception_callback ""}
    } {
        ns_log error "revproxy::upstream: error during establishing connection to $url: $errorMsg"
        if {$exception_callback ne ""} {
            {*}$exception_callback -error $errorMsg -url $url
        }
        foreach chan {frontendChan backendChan} {
            if {[info exists $chan]} {
                ns_connchan close [set $chan]
            }
        }
    }

    nsf::proc gateway_timeout { from url msg } {
        log notice "revproxy: $msg"
        #
        # We received a timeout and we might send a "504 Gateway
        # Timeout" to the client. However, this can be only done, when
        # on the outgoing channel no data was sent so far. In any
        # case, the gateway timeout will be logged and the connection
        # terminated (by the caller).
        #
        foreach entry [ns_connchan list] {
            if {[lindex $entry 0] eq $from} {
                lassign $entry . . . . . sent received
                log notice "gateway_timeout from channel <$entry> sent $sent received $received ($msg)"
                if {$sent == 0} {
                    ns_connchan write $from "HTTP/1.0 504 Gateway Timeout\r\n\r\n504 Gateway Timeout: $url"
                }
            }
        }
    }

    proc channelSetup {chan} {
        if {![info exists ::revproxy::spooled($chan)]} {
            set ::revproxy::spooled($chan) 0
        }
    }

    nsf::proc channelCleanup {{-close:switch false} chan} {
        set closeMsg ""
        if {$close} {
            if {[ns_connchan exists $chan]} {
                ns_connchan close $chan
                set closeMsg "- chan $chan closed"
            }
        }
        if {[info exists ::revproxy::tospool($chan)]} {
            set tospool $::revproxy::tospool($chan)
            unset ::revproxy::tospool($chan)
        } else {
            set tospool ???
        }
        if {[info exists ::revproxy::spooled($chan)]} {
            set spooled $::revproxy::spooled($chan)
            unset ::revproxy::spooled($chan)
        } else {
            set spooled 0
        }
        log notice "cleanup channel $chan, spooled $spooled bytes (to spool $tospool) $closeMsg"
        unset -nocomplain ::revproxy::responseheaderSent($chan)
    }

    #
    # Spool data from $from to $to. The arguments "url" and "arg" might
    # be used for debugging, "condition" is the one-character reason code
    # for calling this function.
    #
    # When this function returns 0, this channel end will be
    # automatically closed.
    #
    nsf::proc spool { from to url timeouts arg condition } {
        log notice "::revproxy::ns_connchan::spool from $from (exists [ns_connchan exists $from]) to $to " \
            "(exists [ns_connchan exists $to]): condition $condition"
        #log notice "... ARG <$arg> URL <$url>"
        if {$condition eq "t"} {
            ::revproxy::ns_connchan::gateway_timeout $from $url "spool timeout occurred while spooling $from to $to"
            log notice "revproxy: spool timeout (MANUAL cleanup on $from to $to needed?)"
            channelCleanup -close $to
            # returning 0 means automatic cleanup on $from
            return 0
        } elseif {$condition ne "r"} {
            log notice "::revproxy::ns_connchan::spool unexpected condition '$condition' while spooling $from to $to"
            #log notice "... timeouts <$timeouts> arg <$arg>"
            #log notice "... read channel status <[ns_connchan status $from]>"
            channelCleanup -close $to
            return 0
        }

        if {[ns_connchan exists $from]} {

            if {$arg ne "" && ![info exists ::revproxy::responseheaderSent($to)]} {
                {*}$arg
                #log notice "sent response header to $to"
                set ::revproxy::responseheaderSent($to) 1
            }
            channelSetup $from

            try {
                ns_connchan read $from
            } on ok {msg} {
                #
                # Everything is OK.
                #
            } trap {POSIX ECONNRESET} {} {
                #
                # The other side has closed the connection. Don't
                # complain and perform standard cleanup.
                #
                log notice "revproxy::spool: ECONNRESET on $from"
                set msg ""
            } on error {errorMsg} {
                ns_log error "revproxy::spool: received error while reading from $from: $errorMsg ($::errorCode)"
                #
                # Drop into the cleanup below
                #
                set msg ""
            }
            if {$msg eq ""} {
                log notice "... auto closing $from manual $to: $url "
                #
                # Close our end ...
                #
                set result 0
                #
                # ... and close as well the other end.
                #
                if {[ns_connchan exists $to]} {
                    #ns_connchan close $to
                    channelCleanup -close $to
                }
            } else {
                #
                # Some data was received, send it to the other end.
                #
                log notice "spool: send [string length $msg] bytes from $from to $to ($url)"

                set result [revproxy::ns_connchan::write $from $to $msg -url $url -timeouts $timeouts]
                if {$result == 2} {
                    #
                    # The write operation to $to was blocked, we have to
                    # suspend the spool callback reading from '$from'.
                    #
                    log notice "PROXY $from: must SUSPEND reading from $from (blocking backend $to) "
                } elseif {$result == 0} {
                    #
                    # The write operation ended in an error. Maybe we
                    # have to close here the channel explicitly.
                    #
                    ns_log notice "revproxy::spool: write $from to $to returned an error; " \
                        "MANUAL cleanup of $to (from $from)"
                    channelCleanup -close $to
                }
            }
        } else {
            log notice "... called on closed channel $from reason $condition"
            set result 0
        }

        # log notice "... return $result"
        return $result
    }

    #
    # revproxy::ns_connchan::write
    #
    nsf::proc write { from to data {-url ""} -timeouts} {
        #
        # return values:
        # *  1: write was successful
        # *  0: write was partial, write callback was submitted
        # * -1: write resulted in an error
        try {
            ns_connchan write -buffered $to $data

        } trap {NS_TIMEOUT} {errorMsg} {
            log notice "spool: TIMEOUT during send to $to ($url) "
            set result 0

        } trap {POSIX EPIPE} {} {
            #
            # A "broken pipe" error might happen easily, when
            # the transfer is aborted by the client. Don't
            # complain about it.
            #
            log notice "write: EPIPE during send to $to ($url) "
            set result 0

        } trap {POSIX ECONNRESET} {} {
            #
            # The other side has closed the connected
            # unexpectedly. This happens when e.g. a browser page is
            # not fully rendered yet, but the user clicked already to
            # some other page. Do not raise an error entry in such
            # cases.
            #
            log notice "write: ECONNRESET during send to $to ($url) "
            set result 0

        } trap {POSIX {unknown error}} {} {
            ns_log warning "revproxy: strange 0 byte write occurred on $to"
            set result 0

        } on error {errorMsg} {
            #
            # all other errors
            #
            ns_log error "revproxy write: error on channel $to: $::errorCode, $errorMsg"
            set result 0

        } on ok {nrBytesSent} {
            set toSend [string length $data]
            # log notice "spool: 'ns_connchan write' wanted to write $toSend bytes, " \
                "wrote $nrBytesSent (sofar $::revproxy::spooled($to))"
            incr ::revproxy::spooled($to) $nrBytesSent
            if {$nrBytesSent < $toSend} {
                #
                # A partial send operation happened.
                #
                # log notice "partial write (send) operation, could only send $nrBytesSent of $toSend bytes"
                #set remaining [string range $data $nrBytesSent end]
                log notice "spool to $to: PARTIAL WRITE ($nrBytesSent of $toSend) " \
                    "register write callback for $to with remaining [expr {$toSend - $nrBytesSent}] bytes " \
                    "(sofar $::revproxy::spooled($to)), setting callback to " \
                    "::revproxy::ns_connchan::write_once timeout [dict get $timeouts -timeout]"
                #
                # On revproxy::ns_connchan::write_once, we do not want to set the
                # sendtimeout for the time being (it would block), the
                # receivetimeout is not necessary; so set just the
                # polltimeout (specified via "-timeout). This timeout
                # can be handled via the "t" flag in the callback
                # proc.
                #
                ns_connchan callback \
                    -timeout [dict get $timeouts -timeout] \
                    $to [list ::revproxy::ns_connchan::write_once $from $to $url $timeouts] wex
                set result 2
            } else {
                #
                # Everything was written.
                #
                #log notice "... write: everything was written to '$to' continue reading from '$from'"
                set result 1
            }
        }
        # log notice "write returns $result"
        return $result
    }

    #
    # revproxy::ns_connchan::write_once
    #
    nsf::proc write_once { from to url timeouts condition } {
        #
        # Callback for writable: writing has blocked before, we
        # suspended reading and wait for flushing out buffer before we
        # continue reading.
        #
        log notice "revproxy::ns_connchan::write_once: condition $condition"

        if {$condition eq "t"} {
            ::revproxy::ns_connchan::gateway_timeout $to $url "timeout occurred while writing once $from to $to"
            log notice "revproxy::ns_connchan::write_once timeout (MANUAL cleanup on $from to $to needed?)"
            channelCleanup -close $from
            # returning 0 means automatic cleanup on $to
            return 0
        } elseif {$condition ne "w"} {
            log warning "revproxy::ns_connchan::write_once unexpected condition '$condition' while writing to $to ($url)"
            return 0
        }

        set buffered_bytes [dict get [ns_connchan status $to] sendbuffer]
        log notice "revproxy::ns_connchan::write_once: want to send $buffered_bytes buffered bytes" \
            "from $from to $to (condition $condition)"

        if {$buffered_bytes == 0} {
            ns_log warning "revproxy::ns_connchan::write_once: have no BUFFERED BYTES during send to $to ($url)" \
                "condition $condition status [ns_connchan status $to]"
            set continue 0
        } else {
            set continue 1

            try {
                ns_connchan write -buffered $to ""

            } trap {POSIX EPIPE} {} {
                #
                # A "broken pipe" error might happen easily, when
                # the transfer is aborted by the client. Don't
                # complain about it.
                #
                log notice "revproxy::ns_connchan::write_once: EPIPE during send to $to ($url) "
                set continue 0

            } trap {POSIX ECONNRESET} {} {
                #
                # The other side has closed the connected
                # unexpectedly. This happens when e.g. a browser page is
                # not fully rendered yet, but the user clicked already to
                # some other page. Do not raise an error entry in such
                # cases.
                #
                log notice "revproxy::ns_connchan::write_once: ECONNRESET during send to $to ($url) "
                set continue 0

            } on error {errorMsg} {
                ns_log error "revproxy::ns_connchan::write_once: returned error: $errorMsg ($::errorCode)"
                set continue 0

            } on ok {nrBytesSent} {
                set status [ns_connchan status $to]
                log notice "revproxy::ns_connchan::write_once: write ok, nrBytesSent <$nrBytesSent> status $status"
            }
        }
        if {$continue == 1} {
            if {$nrBytesSent == 0} {
                ns_log warning "revproxy::ns_connchan::write_once: strangely, we could not write," \
                    "although the socket was writable" \
                    "(still [dict get $status sendbuffer] to send)... trigger again. \nStatus: $status"
                ns_sleep 1ms

            } elseif {[dict get $status sendbuffer] > 0} {
                log notice "revproxy::ns_connchan::write_once was not successful flushing the buffer " \
                    "(still [dict get $status sendbuffer])... trigger again. \nStatus: $status"

            } else {
                #
                # All was sent, fall back to normal read-event driven handler
                #
                log notice "revproxy::ns_connchan::write_once all was written to '$to' resume reading from '$from'"
                #    "\nFROM Status: [ns_connchan status $from]" \
                #    "\n... old callback [dict get [ns_connchan status $from] callback]" \
                #    "\n... new callback [list ::revproxy::spool $from $to $url $timeouts 0]"
                ns_connchan callback \
                    -timeout [dict get $timeouts -timeout] \
                    -sendtimeout [dict get $timeouts -sendtimeout] \
                    -receivetimeout [dict get $timeouts -receivetimeout] \
                    $from [list ::revproxy::ns_connchan::spool $from $to $url $timeouts ""] rex
                #
                # Return 2 to signal to deactivate the writable callback.
                #
                set continue 2
            }
        }
        if {$continue == 0} {
            #
            # There was an error. We must cleanup the "$from" channel
            # manually, the "$to" channel is automaticalled freed.
            #
            log notice "revproxy: write_once MANUAL cleanup of $from (to $to automatic)"
            #ns_connchan close $from
            channelCleanup -close $from
        }
        log notice "revproxy::ns_connchan::write_once returns $continue (from $from to $to)"

        return $continue
    }

    nsf::proc sendResponseHeader {
        -from
        -to
        -url
        -callback
        -status_line
        -header_info
        -body
    } {
        log notice "revproxy::ns_connchan::sendResponseHeader: status line <$status_line>"
        set status [lindex $status_line 1]
        #
        # For most error codes, we want to make sure that the
        # connection is closed after every request. This is currently
        # necessary, since for persistent connections, we can't
        # substitute the request URL inside the stream without
        # continuous parsing.
        #
        # For informational status codes (1xx) there is no need to
        # close the connection (e.g. WebSockets).
        #
        if {$status >= 200} {
            set responseHeaders         [ns_set create {*}$header_info]
            set received_connection     [ns_set iget $responseHeaders connection ""]
            set received_content_length [ns_set iget $responseHeaders content-length ""]

            #
            # In case, a backendResponseCallback is set, call it with
            # "-status" and "-responseHeaders". The callback can modify
            # the ns_set with the response headers, maybe, stripping
            # upstream headers etc.
            #
            if {$callback ne ""} {
                {*}$callback -url $url -responseHeaders $responseHeaders -status $status
            }

            #
            # Make sure to close the connection
            #
            ns_set iupdate $responseHeaders connection close

            #
            # Build the response
            #
            set response $status_line\r\n
            set size [ns_set size $responseHeaders]
            for {set i 0} {$i < $size} {incr i} {
                append response "[ns_set key $responseHeaders $i]: [ns_set value $responseHeaders $i]\r\n"
            }
            log notice "revproxy::ns_connchan::sendResponseHeader: from $url\n$response"
            if {$received_content_length ne ""} {
                # log notice "revproxy::ns_connchan::sendResponseHeader: set tospool($to) -> $received_content_length"
                set ::revproxy::tospool($to) $received_content_length
            }
            #log notice "===== revproxy::ns_connchan::sendResponseHeader content-length '$received_content_length' connection '$received_connection'"
            set headerLength [string length $response]
            append response \r\n$body
            set toWrite [string length $response]

            set written [ns_connchan write $to $response]
            incr ::revproxy::spooled($to) [expr {$written - ($headerLength + 2)}]
            log notice "revproxy::ns_connchan::sendResponseHeader: from $from to $to towrite $toWrite written $written " \
                "spooled($to) $::revproxy::spooled($to)"
            #record $to-rewritten $response

        } else {
            #
            # e.g. HTTP/1.1 101 Switching Protocols
            #
            set header [join [lmap {key value} $header_info {string cat $key: " " $value}] \r\n]
            log notice SWITCH status_line $status_line $header
            ns_connchan write $to [string cat $status_line \r\n $header \r\n\r\n $body]
        }
    }

    #
    # Handle backend replies in order to be able to post-process the
    # response header fields. This is e.g. necessary to downgrade to
    # HTTP/1.0 requests.
    #
    nsf::proc backendResponse {
        {-callback ""}
        -sendtimeout
        -receivetimeout
        from
        to
        url
        timeouts
        arg
        condition
    } {
        log notice "::revproxy::ns_connchan::backendResponse from $from condition '$condition' server '[ns_info server]'"
        if { $condition eq "r" } {
            #
            # Read from backend
            #
            channelSetup $from
            channelSetup $to
            set msg [ns_connchan read $from]

        } elseif { $condition eq "t" } {
            #
            # Connection Timeout
            #
            ::revproxy::ns_connchan::gateway_timeout $to $url "connection timeout occurred while waiting for backend response $from to $to"
            channelCleanup -close $to
            set msg ""

        } else {
            log notice "::revproxy::ns_connchan::backendResponse unexpected condition '$condition' while processing backend response"
            set msg ""
        }

        if {$msg eq ""} {
            log notice "::revproxy::ns_connchan::backendResponse: ... auto closing $from $url"
            #
            # Close our end ...
            #
            set result 0
            #
            # ... and close as well the other end.
            #
            channelCleanup -close $to
        } else {
            #
            # Receive response from backend. We assume, that we can
            # receive the header of the response in one sweep, ... which
            # seems to be the case on our tested systems.
            #
            log notice "::revproxy::ns_connchan::backendResponse: received response [string length $msg] bytes from $from to $to ($url)"
            #record $to $msg

            try {
                ns_parsemessage $msg
            } on error {errorMsg} {
                log notice "::revproxy::ns_connchan::backendResponse: could not parse header <$msg>"
                set result 0
                channelCleanup -close $to
            } on ok {d} {
                dict with d {
                    #
                    # We cannot pass the ns_set directly, since ns_sets
                    # are cleaned up after every event in the socks
                    # thread. Therefore, we pass it in a attribute-value
                    # list via "header_info".
                    #
                    #log notice "::revproxy::ns_connchan::backendResponse: received headers [ns_set format $headers]"
                    set cmd [list sendResponseHeader \
                                 -from $from -to $to -url $url \
                                 -callback $callback \
                                 -status_line $firstline \
                                 -header_info [ns_set array $headers] \
                                 -body $body]

                    set received_connection     [string tolower [ns_set iget $headers connection ""]]
                    set received_content_length [ns_set iget $headers content-length ""]
                }
                if {$received_content_length eq "" || $received_content_length eq "0"} {
                    #
                    # In case there is (potentially) no content, send
                    # the received upstream headers now, since the
                    # read callback on this socket might not be
                    # triggered anymore.
                    #
                    {*}$cmd
                    set cmd ""
                } else {
                    #
                    # Otherwise, delay cmd until we receive some data
                    # via callback in revproxy::ns_connchan::spool.
                    # We need to delay cmd to be able to report to the
                    # client a potential timeout with the appropriate
                    # status code. Otherwise, it would be too late.
                    #
                }

                if {$received_content_length eq "" && $received_connection in {"close" "upgrade"}} {
                    #
                    # Assume streaming HTML or a websocket upgrade.
                    #
                    set timeout [expr {$received_connection eq "close"
                                       ? [ns_config ns/server/[ns_info server]/module/revproxy streaminghtmltimeout 2m]
                                       : "1y"}]
                    log notice "Streaming response detected, changing poll timeout on frontend $to and backend $from to $timeout value"

                    dict set timeouts -timeout $timeout

                    #
                    # Get the old callback and rewrite just the timeout values
                    #
                    set callback_info [ns_connchan status $to]
                    set callback [dict get $callback_info callback]
                    lset callback 4 $timeouts

                    ns_connchan callback \
                        -timeout [dict get $timeouts -timeout] \
                        -sendtimeout [dict get $timeouts -sendtimeout] \
                        -receivetimeout [dict get $timeouts -receivetimeout] \
                        $to $callback [dict get $callback_info condition]

                }

                #
                # Change the callback from begin of request handling
                # to regular spooling for future requests.
                #
                ns_connchan callback \
                    -timeout [dict get $timeouts -timeout] \
                    -sendtimeout [dict get $timeouts -sendtimeout] \
                    -receivetimeout [dict get $timeouts -receivetimeout] \
                    $from [list ::revproxy::ns_connchan::spool $from $to $url $timeouts $cmd] rex

                set result 1
            }
        }

        # log notice "... return $result"
        return $result
    }

    interp alias {} [namespace current]::log {} ::revproxy::log

}

#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
