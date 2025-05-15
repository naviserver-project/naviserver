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
# Tcl module for NaviServer to use it as a reverse proxy server.
#

#
# Revproxy (as implemented) requires NSF. Revproxy is recommended, but
# not strictly necessary for running NaviServer.
#
if {[info commands ::nsf::proc] eq ""} {
    ns_log warning "NSF is not installed. The revproxy is not available"
    return
}

ns_log notice "Using NSF version [package require nsf]"

if {$::tcl_version eq "8.5"} {
    #
    # In Tcl 8.5, "::try" was not yet a built-in of Tcl
    #
    package require try
}

#
# This file can be installed via "make install" as a Tcl module in
# /usr/local/ns/tcl/revproxy/revproxy-procs.tcl and registered as a
# Tcl module in the NaviServer configuration file.
#

namespace eval ::revproxy {
    variable version
    variable verbose
    variable register_callbacks

    set version 0.21
    set verbose [ns_config ns/server/[ns_info server]/module/revproxy verbose 0]

    #ns_logctl severity Debug(connchan) on

    #
    # Upstream/backend handler (deliver request from an upstream/backend server)
    #
    # Serve the requested file from an upstream server, which might be
    # an HTTP or HTTPS server.

    nsf::proc upstream {
        when
        -target:required
        {-backend_reply_callback ""}
        {-backend_response_callback ""}
        {-backendconnection ""}
        {-connecttimeout 1s}
        {-exception_callback "::revproxy::exception"}
        {-insecure:switch}
        {-receivetimeout ""}
        {-regsubs:0..n ""}
        {-response_header_callback ""}
        {-sendtimeout ""}
        {-targethost ""}
        {-timeout 1m}
        {-url_rewrite_callback "::revproxy::rewrite_url"}
        {-use_target_host_header:boolean false}
        {-validation_callback ""}
    } {
        #
        # @param when indicates, how the callback was invoked. When
        #        the callback was registered via filter, typical
        #        values are "preauth" or postauth. When registered as
        #        a proc, the value will be "proc"
        #

        if {$backend_reply_callback ne ""} {
            ns_deprecated "... ns_http-backend_response_callback ..."
            set $backend_response_callback $backend_reply_callback
        }
        if {$backendconnection eq ""} {
            set backendconnection \
                [ns_config ns/server/[ns_info server]/module/revproxy backendconnection ns_http+ns_connchan]
        }
        set extraArgs {}
        set spoolResponse true
        switch $backendconnection {
            ns_connchan { }
            ns_http     { set spoolResponse false }
            ns_http+ns_connchan {
                set backendconnection ns_http
            }
            default {
                set backendconnection ns_connchan
                ns_log warning "revproxy: unknown backend connection type '$backendconnection';" \
                        "fall back to $backendconnection"
            }
        }
        set extraArgs [list -spoolresponse $spoolResponse]
        if {$insecure} {
            lappend extraArgs -insecure
        }

        if {[llength $target] > 0} {
            set md5 [ns_md5 $target]
            set count [nsv_incr module:revproxy:proxyset $md5]
            # log notice "===== upstream [ns_info server] <$target> count $count"
            set target [lindex $target [expr {$count % [llength $target]}]]
        }
        nsv_incr module:revproxy:target $target
        log notice "request on server '[ns_info server]' using '$backendconnection $extraArgs' connection ===== [ns_conn method] $target"

        set requestHeaders [ns_conn headers]
        if {[ns_set iget $requestHeaders upgrade] eq "websocket"} {
            #
            # We received a WebSocket upgrade response from the
            # server. WebSocket use long open connections, we can
            # support these only via "ns_connchan", since "ns_http"
            # gets the data in one sweep. So force the ns_connchan
            # handler if necessary.
            #
            log notice "WebSocket upgrade, forcing long timeouts"
            #
            # Using ns_connchan always should not be necessary, but
            # ns_http does not handle "101 Switching Protocols".
            #
            set useConnchanAlways 1
            if {$useConnchanAlways || !$spoolResponse} {
                log notice "switch backend connection from '$backendconnection'" \
                    "to 'ns_connchan', since a WebSocket upgrade was received."
                set backendconnection ns_connchan
            }
            #
            # WebSockets are long running requests, where no data
            # might be received for a long time with large
            # intervals. Therefore, we force a really long timeout.
            #
            set timeout 1y
        }

        #
        # Assemble URL in two steps:
        #   - First, perform regsubs on the current URL (if provided)
        #   - Second, compose the final URL for the backend via the default
        #     handler or a custom callback
        #
        set url [ns_conn url]
        if {[llength $regsubs] > 0} {
            #
            # When "regsubs" is provided, it has to be a list of pairs
            # with two elements, the "from" regexp and the "to"
            # substitution pattern. By providing a list of regsubs,
            # multiple substitutions can be performed.
            #
            foreach regsub $regsubs {
                if {[llength $regsub] == 2} {
                    lassign $regsub from to
                    #log notice "regsub $from $url $to url"
                    regsub $from $url $to url
                } else {
                    log warning "revproxy::upstream: invalid regsub pair <$regsub>;" \
                        "every entry must contain 2 elements"                }
            }
        }

        #
        # Compute the final upstream URL based on the updated URL and
        # the query parameters.
        #
        set url [{*}$url_rewrite_callback \
                     -target $target \
                     -url $url \
                     -query [ns_conn query] \
                     -fragment [ns_conn fragment] \
                    ]
        #log notice "===== submit via ${backendconnection}, [ns_conn method] $url"

        #
        # When the callback decides, we have to cancel this request,
        # it can send back an empty URL. The callback can terminate
        # the connection via an "ns_returnforbidden" before returning
        # the empty string.
        #
        if {$url eq ""} {
            return filter_return
        }

        #
        # Finally, start the transmission to the backend via the
        # configured means.
        #
        return [::revproxy::${backendconnection}::upstream \
                    -url $url \
                    -timeout $timeout \
                    -connecttimeout $connecttimeout \
                    -sendtimeout $sendtimeout \
                    -receivetimeout $receivetimeout \
                    -validation_callback $validation_callback \
                    -exception_callback $exception_callback \
                    -backend_response_callback $backend_response_callback \
                    -response_header_callback $response_header_callback \
                    -request [::revproxy::request -targethost $targethost] \
                    -use_target_host_header $use_target_host_header\
                    {*}$extraArgs
                   ]
    }

    nsf::proc ::revproxy::request {{-targethost ""}} {
        set requestHeaders [ns_conn headers]

        #
        # For debugging, it might be easier to avoid compressed data.
        #
        #ns_set delkey -nocase $requestHeaders Accept-Encoding

        #
        # Add extra "forwarded" header fields, i.e. "x-forwarded-for", "via",
        # "x-forwarded-proto", and "x-ssl-request" (if appropriate).
        #
        set XForwardedFor [split [ns_set iget $requestHeaders "x-forwarded-for" ""] " ,"]
        set XForwardedFor [lmap e $XForwardedFor {if {$e eq ""} continue; set e}]
        lappend XForwardedFor [ns_conn peeraddr]
        ns_set iupdate $requestHeaders x-forwarded-for [join $XForwardedFor ","]

        #
        # Use via pseudonym based on server name and pid (should be
        # sufficient to detect loops)
        #
        set via [split [ns_set iget $requestHeaders "via" ""] ","]
        set via [lmap e $via {if {$e eq ""} continue; string trim $e}]
        lappend via "[ns_conn version] [ns_info server]-[pid]"
        ns_set iupdate $requestHeaders via [join $via ","]

        set proto [ns_conn protocol]
        ns_set iupdate $requestHeaders x-forwarded-proto $proto
        if {$proto eq "https"} {
            ns_set iupdate $requestHeaders x-ssl-request 1
        }

        if {$targethost ne ""} {
            ns_set iupdate $requestHeaders host $targethost
        }
        log notice [ns_set format $requestHeaders]

        #
        # Build dictionary "request" containing the request data
        #
        dict set request headers $requestHeaders
        dict set request method [ns_conn method]
        dict set request version [ns_conn version]
        dict set request requester [ns_conn location]

        set contentType   [ns_set iget $requestHeaders content-type]
        set contentLength [ns_set iget $requestHeaders content-length ""]
        set binary false

        if {$contentType ne ""
            && [ns_encodingfortype $contentType] eq "binary"} {
            set binary true
        }
        dict set request binary $binary

        set contentfile [ns_conn contentfile]
        if {$contentfile ne ""} {
            dict set request contentfile $contentfile
            if {$contentLength eq ""} {
                set computedContentLength [file size $contentfile]
            }
        } else {
            if {$binary} {
                dict set request content [ns_conn content -binary]
            } else {
                dict set request content [ns_conn content]
            }
            set computedContentLength [string length [dict get $request content]]
        }

        if {$contentLength eq "" && $computedContentLength > 0} {
            log notice "adding missing content-length $computedContentLength"
            ns_set iupdate $requestHeaders content-length $computedContentLength
        }

        return $request
    }


    nsf::proc upstream_send_failed {
        {-status 503}
        -errorMsg
        -frontendChan
        -backendChan
        -url
        {-exception_callback ""}
        {-severity error}
    } {
        #
        # When a frontendChan or backendChan is provided, these are closed.
        #
        log notice "revproxy::upstream: send failed URL $url '$errorMsg'"
        if {$exception_callback ne ""} {
            {*}$exception_callback -status $status -error $errorMsg -url $url \
                -frontendChan [expr {[info exists frontendChan] ? $frontendChan : ""}]
        }
        foreach chan {frontendChan backendChan} {
            if {[info exists $chan]} {
                ns_connchan close [set $chan]
            }
        }
    }

    #
    # Default exception handler for reporting error messages to the
    # browser.
    #
    nsf::proc exception {
        {-status 503}
        {-msg ""}
        -error
        -url
        {-frontendChan ""}
    } {
        if {$msg eq ""} {
            ns_log notice "revproxy exception backend with URL '$url' failed with status $status"
            set msg "Backend error: [ns_quotehtml $error]"
        }
        if {$frontendChan ne ""} {
            switch $status {
                502 {set phrase "Bad Gateway"}
                503 {set phrase "Service Unavailable"}
                504 {set phrase "Gateway Timeout"}
                default {set phrase Error}
            }
            try {
                ns_connchan write $frontendChan "HTTP/1.0 $status $phrase\r\n\r\n$status $phrase: $url"
            } trap {NS_TIMEOUT} {} {
                ns_log notice "::revproxy:exception: TIMEOUT during send to $frontendChan"
            } trap {POSIX EPIPE} {} {
                ns_log notice "::revproxy::exception:  EPIPE during send to $frontendChan"
            } trap {POSIX ECONNRESET} {} {
                ns_log notice "::revproxy::exception:  ECONNRESET during send to $frontendChan"
            } on error {errorMsg} {
                ns_log warning "::revproxy::exception:  other error during send to $frontendChan: $errorMsg"
            } on ok {result} {
            }

        } elseif [ns_conn isconnected] {
            ns_returnerror $status $msg
        } else {
            ns_log error "revproxy exception (no return channel open): $status '$msg'"
        }
    }

    #
    # Default rewrite_url handler for composing upstream target URL
    # based on the target (containing at typically the protocol and
    # location), the incoming URL and the actual query parameter form
    # the incoming URL.
    #
    nsf::proc rewrite_url { -target -url {-query ""} {-fragment ""}} {
        set url [string trimright $target /]/[string trimleft $url /.]
        if {$query ne ""} {append url ?$query}
        if {$fragment ne ""} {append url #$fragment}
        return $url
    }


    #
    # Simple logger for system log, evaluating the ::revproxy::verbose
    # variable.
    #
    nsf::proc log {severity args} {
        if {$::revproxy::verbose} {
            ns_log $severity "PROXY: " {*}$args
        }
    }

    #
    # Simple recorder for writing content to a file (for
    # debugging). The files are saved in the tmp directory, under a
    # folder named by the PID, channel identifier are used as file
    # names.
    #
    nsf::proc record {chan msg} {
        file mkdir [ns_config ns/parameters tmpdir]/[ns_info pid]
        set fn [ns_config ns/parameters tmpdir]/[ns_info pid]/$chan
        set F [open $fn a]
        puts -nonewline $F $msg
        close $F
    }

    #
    # Get configured URLs
    #
    set register_callbacks [ns_config ns/server/[ns_info server]/module/revproxy register]
    if {$register_callbacks eq ""} {
        set register_callbacks [ns_config ns/server/[ns_info server]/module/revproxy filters]
        if {$register_callbacks ne ""} {
            ns_log warning "Using deprecated configuration parameter 'filters';" \
                "use parameter 'register' instead"
        }
    }
    if {$register_callbacks ne ""} {
        ns_log notice "revproxy registers callbacks\n$register_callbacks"
        eval $register_callbacks
    }

    foreach s [ns_configsections] {
        if {[string match ns/server/[ns_info server]/module/revproxy/* [ns_set name $s]]} {

            set defaults {}
            foreach parameter {
                type
                target
                backend_response_callback
                response_header_callback
                backendconnection
                connecttimeout
                exception_callback
                insecure
                receivetimeout
                regsubs
                sendtimeout
                targethost
                timeout
                url_rewrite_callback
                use_target_host_header
                validation_callback
            } {
                if {[ns_set find $s $parameter] != -1} {
                    #ns_log notice REVPROXY [ns_set name $s] $parameter [ns_set get -all $s $parameter]
                    lappend defaults -$parameter [ns_set get $s $parameter]
                }
            }
            set constraints ""
            foreach c [ns_set iget -all $s constraints] {
                if {[llength $c] % 2 != 0} {
                    ns_log warning "ignore invalid constraints (most be key/value list): '$c'"
                } else {
                    lappend constraints $c
                }
            }

            foreach map [ns_set get -all $s map] {
                lassign $map method path params
                set arguments [dict merge $defaults $params]
                set type proc
                if {[dict exists $arguments -type]} {
                    set type [dict get $arguments -type]
                    if {$type ni {proc preauth postauth}} {
                        ns_log error "revproxy: invalid revproxy type: '$type'; mapping ignored: $map"
                        continue
                    }
                    dict unset arguments -type
                } elseif {[dict exists $arguments -insecure]} {
                    dict unset arguments -insecure
                    lappend arguments -insecure
                }
                if {$type eq "proc"} {
                    set baseCmd [list ns_register_proc $method $path ::revproxy::upstream $type {*}$arguments]
                } else {
                    set baseCmd [list ns_register_filter $type $method $path ::revproxy::upstream {*}$arguments]
                }

                if {[llength $constraints] == 0} {
                    set constraints [list ""]
                }
                foreach c $constraints {
                    set cmd [expr {$c ne "" ? [linsert $baseCmd 1 -constraints $c] : $baseCmd}]
                    ns_log notice "REVPROXY [ns_set name $s]:\n$cmd\n"
                    eval $cmd
                }
            }
        }
    }

    ns_log notice "revproxy module version $version loaded for server '[ns_info server]'" \
        "using [ns_config ns/server/[ns_info server]/module/revproxy backendconnection ns_connchan]"
}


#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
