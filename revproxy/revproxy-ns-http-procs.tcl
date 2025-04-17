#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
#-------------------------------------------------------------------------------
# Implementation of the reverse proxy backend connection vis "ns_http"
#-------------------------------------------------------------------------------

#
# Revproxy (as implemented) requires NSF. Revproxy is recommended, but
# not strictly necessary for running NaviServer.
#
if {[info commands ::nsf::proc] eq ""} {
    ns_log warning "NSF is not installed. The revproxy is not available"
    return
}

namespace eval ::revproxy {}
namespace eval ::revproxy::ns_http {

    nsf::proc upstream {
        -url:required
        {-insecure:switch}
        {-timeout 105.0s}
        {-connecttimeout 1s}
        {-sendtimeout ""}
        {-receivetimeout ""}
        {-validation_callback ""}
        {-exception_callback "::revproxy::exception"}
        {-backend_response_callback ""}
        -request:required
        {-spoolresponse true}
        {-use_target_host_header:boolean false}
    } {
        #
        # @param receivetimeout ignored
        #        (just for interface compatibility with the ns_connchan variant)
        # @param sendtimeout ignored
        #        (just for interface compatibility with the ns_connchan variant)
        #
        if {$sendtimeout ne ""} {
            ns_log warning "::revproxy::ns_http::upstream: sendtimeout '$sendtimeout' ignored"
        }
        if {$receivetimeout ne ""} {
            ns_log warning "::revproxy::ns_http::upstream: receivetimeout '$receivetimeout' ignored"
        }

        #
        # Perform the transmission via ns_connchan... but before this,
        # call the validation callback on the final result. The
        # callback invocation is in the delivery-method specific code,
        # since the delivery method might add headers (see ns_connchan variant).
        #
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

        set extraArgs {}
        set requestHeaders [dict get $request headers]
        set method         [dict get $request method]
        set binary         [dict get $request binary]

        if {$insecure} {
            lappend extraArgs -insecure
        }
        if {[dict exists $request contentfile]} {
            lappend extraArgs -body_file [dict get $request contentfile]
        } else {
            lappend extraArgs -body [dict get $request content]
        }

        if {$binary} {
            lappend extraArgs -binary
        }

        if {$spoolresponse} {
            #
            # This switch tells us to spool the data from backend as
            # it arrives to the client via connchan. Per default, the
            # ns_http request runs in this case in the background.
            #
            set connchan [ns_connchan detach]

            #if {[string match "*atl.general*batch.js*" $url]} {
            #    ns_connchan debug $connchan 1
            #    ns_log notice "ACTIVATE verbosity: send data to the client via $connchan (URL $url) "
            #}
            lappend extraArgs -outputchan $connchan -spoolsize 0 -raw -response_header_callback ::revproxy::ns_http::responseheaders
            #ns_log notice "send data to the client via $connchan (URL $url)"
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

        # We see the final headers best, when "Debug(request)" is on in the line
        # ... Debug(request): full request ....
        #log notice "outgoing request headers sent via ns_http [ns_set format $requestHeaders]"
        #ns_set print $requestHeaders

        set partialresultsFlag [expr {[ns_info version]>=5 ?  "-partialresults" : ""}]
        set connchanArg [expr {[info exists connchan] ? [list -connchan $connchan] : ""}]
        set expiretimeout 1d
        set timeouts [list \
                          -connecttimeout $connecttimeout \
                          -expiretimeout $expiretimeout \
                          -timeout $timeout \
                         ]

        set doneArgs [list -url $url {*}$connchanArg {*}$partialresultsFlag \
                          -backend_response_callback $backend_response_callback \
                          -exception_callback $exception_callback \
                          -timeouts $timeouts \
                      ]

        set queue 1
        if {!$spoolresponse && $queue} {
            log notice "cannot run ns_http in the background for now, since delivery requires to be connected"
            set queue 0
        }

        set keepHostHeaderArg [expr {$use_target_host_header ? "" : "-keep_host_header"}]
        if {$queue} {
            set done_callbback [list ::revproxy::ns_http::done {*}$doneArgs]

            log notice             ns_http queue \
                {*}$partialresultsFlag \
                {*}$unixSocketArg \
                {*}$keepHostHeaderArg \
                -spoolsize 100kB \
                -method $method \
                -headers $requestHeaders \
                -connecttimeout $connecttimeout \
                -timeout $timeout \
                -expire $expiretimeout \
                {*}$extraArgs \
                -done_callback $done_callbback \
                $url

            ns_http queue \
                {*}$partialresultsFlag \
                {*}$unixSocketArg \
                {*}$keepHostHeaderArg \
                -spoolsize 100kB \
                -method $method \
                -headers $requestHeaders \
                -connecttimeout $connecttimeout \
                -timeout $timeout \
                -expire $expiretimeout \
                {*}$extraArgs \
                -done_callback $done_callbback \
                $url

        } else {

            try {
                #log notice             ns_http run \
                    {*}$partialresultsFlag \
                    {*}$unixSocketArg \
                    {*}$keepHostHeaderArg \
                    -spoolsize 100kB \
                    -method $method \
                    -headers $requestHeaders \
                    -connecttimeout $connecttimeout \
                    -timeout $timeout \
                    -expire $expiretimeout \
                    {*}$extraArgs \
                    $url

                ns_http run \
                    {*}$partialresultsFlag \
                    {*}$unixSocketArg \
                    {*}$keepHostHeaderArg \
                    -spoolsize 100kB \
                    -method $method \
                    -headers $requestHeaders \
                    -connecttimeout $connecttimeout \
                    -timeout $timeout \
                    -expire $expiretimeout \
                    {*}$extraArgs \
                        $url

            } trap {NS_TIMEOUT} {r} {
                ::revproxy::ns_http::done {*}$doneArgs NS_TIMEOUT $r

            } on ok {r} {
                ::revproxy::ns_http::done {*}$doneArgs 1 $r

            } on error {errorMsg} {
                ::revproxy::ns_http::done {*}$doneArgs 0 $errorMsg

            }
        }
        return filter_return
    }

}

nsf::proc ::revproxy::ns_http::done {
    -connchan
    -url
    -timeouts
    -partialresults:switch
    {-backend_response_callback ""}
    {-exception_callback $exception_callback}
    result
    d
} {
    if {$result != 0} {
        #
        # The behaviour when called via "done_callback" is slightly
        # different to a direct call without the callback: In the
        # callback case we see just "0" or "1" as result, we have to
        # get the NS_TIMEOUT result code from the result dict.
        #
        if {[dict exists $d state] && [dict get $d state] eq "NS_TIMEOUT"} {
            set result NS_TIMEOUT
        }
    }
    #ns_log notice !!!!! DONE ::revproxy::ns_http::done result <$result> d <$d>

    switch $result {
        NS_TIMEOUT {
            ns_log notice ::revproxy::ns_http::done =========================================== TIMEOUT (connchan [info exists connchan])

            ns_log notice "::revproxy::ns_http::done timeouts $timeouts during send to $url ($d) "
            if {$partialresults && [dict exists $d error]} {
                #
                # This request was sent with "-partialresults" enabled
                #
                set errorMsg [dict get $d error]
                set responseHeaders [dict get $d headers]
                #ns_log notice ".... timeout happened after [dict get $d time] seconds"
                #log notice "RESULT contains error: '$errorMsg' /$::errorCode/\n$d"

                if {![info exists connchan]
                    && [ns_set size $responseHeaders] > 0
                    && [ns_set iget $responseHeaders content-length ""] eq ""
                } {
                    #
                    # We have a timeout, we are not using connhchan
                    # (which supports streaming HTML), and we have no
                    # content length. This is potentially a streaming
                    # HTML request, which cannot be handled by ns_http
                    # without the ns_connchan output channel. Diagnose
                    # this condition in the log file, but still report
                    # an error back to the client.
                    #
                    ns_log error "revproxy: streaming HTML attempt detected on ns_http for URL $url." \
                        "Please switch for this request to the ns_connchan interface!" <[ns_set format $responseHeaders]>
                    set errorMsg "streaming HTML not supported on this interface"
                }
            } else {
                set errorMsg $d
            }

            #log notice "TIMEOUT during send to $url ($errorMsg) "
            ::revproxy::upstream_send_failed \
                -status 504 \
                -errorMsg $errorMsg \
                -url $url \
                -frontendChan [expr {[info exists connchan] ? $connchan : ""}] \
                -exception_callback $exception_callback
        }
        0 {
            #
            # Success case
            #
            #log notice ::revproxy::ns_http::done =========================================== SUCCESS (connchan [info exists connchan])

            if {[info exists connchan] && [ns_connchan exists $connchan]} {
                #
                # In the connchan case (using outputchan), the data
                # has already been transferred.
                #
                ::revproxy::ns_http::drain $connchan
            }

            set responseHeaders [dict get $d headers]
            set status          [dict get $d status]
            #ns_log notice "RESULT of request: $d"
            #ns_log notice "... response headers  <$responseHeaders> <[ns_set array $responseHeaders]>"

            if {$backend_response_callback ne ""} {
                {*}$backend_response_callback -url $url -responseHeaders $responseHeaders -status $status
            }
            #log notice ::revproxy::ns_http::done =========================================== SUCCESS (connected [ns_conn isconnected])
            if {[ns_conn isconnected]} {
                #
                # In the "connected" case, we have no connchan.
                # The headers and the result has to be transferred to the client.
                #
                # We have no outputchan.
                #
                #log notice CONNECTED, keys of result dict: [lsort [dict keys $d]]

                set outputHeaders [ns_conn outputheaders]
                foreach {key value} [ns_set array $responseHeaders] {
                    if {[string tolower $key] ni {
                        connection date server
                        content-length content-encoding
                    }} {
                        ns_set put $outputHeaders $key $value
                        log notice "adding to output headers: <$key> <$value>"
                    }
                }
                #ns_set iupdate $outputHeaders Connection close
                log notice status code $status [ns_set format $outputHeaders]

                #
                # Pass the status code
                #
                ns_headers $status

                #
                # Get the content either as a string or from a spool
                # file (to avoid memory bloats on huge files).
                #
                if {[dict exists $d body]} {
                    log notice "submit string (length [string length [dict get $d body]])"
                    ns_writer submit [dict get $d body]
                } elseif {[dict exists $d file]} {
                    log notice "submit file <[dict get $d file]>"
                    ns_writer submitfile [dict get $d file]
                    file delete [dict get $d file]
                } else {
                    error "REVERSE PROXY <$url>: invalid return dict with keys <[dict keys $d]"
                }
            }
        }
        1 {
            #
            # Error case
            #
            set errorMsg [expr {[dict exists $d error] ? [dict get $d error] : $d}]
            #log notice ============================================ ERROR (connchan [info exists connchan])

            set logmsg "::revproxy::ns_http::upstream: request to URL <$url> returned [list $errorMsg]"
            set silentFlag {}
            if {[info exists connchan]} {
                set statusDict [ns_connchan status $connchan]
                set sendError [dict get $statusDict senderror]
                if {$sendError eq "ECONNRESET"} {
                    # peer has closed connection
                    catch {ns_connchan close $connchan}
                    unset connchan
                    set severityFlag {-severity notice}
                }
                append logmsg " statusDict [list $statusDict]"
            }
            log notice $logmsg
            ::revproxy::upstream_send_failed \
                -errorMsg $errorMsg \
                -url $url \
                {*}[expr {[info exists connchan] ? [list -frontendChan $connchan] : {}}] \
                {*}$silentFlag \
                -exception_callback $exception_callback
        }
    }
}

nsf::proc ::revproxy::ns_http::drain {channel {-done_callback ""}} {
    #
    # Drain and close the connchan channel. In case, there is some
    # unsent data, send it before closing.
    #
    if {[dict get [ns_connchan status $channel] sendbuffer] > 0} {
        ns_log notice "revproxy ns_http+ns_connchan: final buffer of $channel is not empty:" \
            [ns_connchan status $channel]
        #
        # ::revproxy::ns_http::drain_sendbuf will automatically close $channel
        #
        ns_connchan callback $channel \
            [list ::revproxy::ns_http::drain_sendbuf $channel -done_callback ""] wex
    } else {
        ns_connchan close $channel
    }
}


nsf::proc ::revproxy::ns_http::drain_sendbuf {channel {-done_callback ""} when} {
    #
    # This is a callback proc, which is called when the channel
    # becomes writable to send the remaining data. When everything is
    # sent, the callback is de-registered and the channel is closed.
    #
    set result -1
    try {
        ns_connchan write $channel ""
    } trap {NS_TIMEOUT} {} {
        ns_log notice "::revproxy::ns_http::drain_sendbuf: TIMEOUT during send to $channel"
    } trap {POSIX EPIPE} {} {
        ns_log notice "::revproxy::ns_http::drain_sendbuf:  EPIPE during send to $channel"
    } trap {POSIX ECONNRESET} {} {
        ns_log notice "::revproxy::ns_http::drain_sendbuf:  ECONNRESET during send to $channel"
    } on error {errorMsg} {
        ns_log warning "::revproxy::ns_http::drain_sendbuf:  other error during send to $channel: $errorMsg"
    } on ok {result} {
    }
    set status [ns_connchan status $channel]
    log notice "::revproxy::ns_http::drain_sendbuf when '$when' sent $result status $status"
    if {$result == -1} {
        #
        # An unrecoverable condition occurred, close the channel and
        # deregister the callback.
        #
        ns_connchan close $channel
        log notice "::revproxy::ns_http::drain_sendbuf Could call callback <$done_callback>"
        set continue 0
    } elseif {$result == 0 || [dict get $status sendbuffer] > 0} {
        if {$result == 0} {
            ns_log warning "::revproxy::ns_http::drain_sendbuf  was not successful draining the buffer " \
                "(still [dict get $status sendbuffer])... trigger again. status: $status"
        } else {
            ns_log notice "::revproxy::ns_http::drain_sendbuf still [dict get $status sendbuffer] to drain"
        }
        set continue 1
    } else {
        #
        # All was sent, close the channel, call the callback and
        # deregister the callback.
        #
        ns_connchan close $channel
        log notice "::revproxy::ns_http::drain_sendbuf Could call callback <$done_callback>"
        set continue 0
    }

    log notice "::revproxy::ns_http::drain_sendbuf returns $continue (channel $channel)"
    return $continue
}


proc ::revproxy::ns_http::responseheaders {dict} {
    #
    # Send response headers from backend server to revproy client.
    # This function is called by "response_header_callback".
    #
    #ns_log warning !!!! ::revproxy::ns_http::responseheaders

    #ns_logctl severity Debug(task) on
    #ns_logctl severity Debug(connchan) on

    dict with dict {
        foreach key {status phrase headers outputchan} {
            if {![info exists $key] } {
                error "::revproxy::ns_http::responseheaders: missing key '$key' in provided dict: '$dict'"
            }
        }
        if {![ns_connchan exists $outputchan]} {
            error "::revproxy::ns_http::responseheaders: provided channel is not a a connection channel: '$outputchan'"
        }

        #log notice ::revproxy::ns_http::responseheaders <$dict> set $headers <[ns_set array $headers]>
        if {0} {
            foreach {key value} [ns_set array $headers] {
                if {[string tolower $key] ni {
                    xconnection date server
                    xcontent-length xcontent-encoding
                }} {
                    ns_set put $headers $key $value
                    log notice adding to output headers: $key: '$value'
                }
            }
        }

        set response "HTTP/1.1 $status $phrase\r\n"
        foreach {key value} [ns_set array $headers] {
            append response "$key: $value\r\n"
        }
        append response \r\n

        log notice ::revproxy::ns_http::responseheaders \n$response
        set toWrite [string length $response]
        set written -1
        try {
            ns_connchan write $outputchan $response
        } trap {NS_TIMEOUT} {} {
            ns_log notice "::revproxy::ns_http::responseheaders: TIMEOUT during send to $outputchan"
        } trap {POSIX EPIPE} {} {
            ns_log notice "::revproxy::ns_http::responseheaders:  EPIPE during send to $outputchan"
        } trap {POSIX ECONNRESET} {} {
            ns_log notice "::revproxy::ns_http::responseheaders:  ECONNRESET during send to $outputchan"
        } on error {errorMsg} {
            ns_log warning "::revproxy::ns_http::responseheaders:  other error during send to $outputchan: $errorMsg"
        } on ok {written} {
        }
        if {$toWrite != $written} {
            log notice ::revproxy::ns_http::responseheaders towrite $toWrite written $written
        }
    }
}

namespace eval ::revproxy::ns_http {
    interp alias {} [namespace current]::log {} ::revproxy::log
}
#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
