#
# Tcl module for NaviServer to use it as a reverse proxy server.
#

package require nsf

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
    variable filters

    set version 0.20
    set verbose [ns_config ns/server/[ns_info server]/module/revproxy verbose 0]

    #ns_logctl severity Debug(connchan) on

    #
    # Upstream/backend handler (deliver request from an upstream/backend server)
    #
    # Serve the requested file from an upstream server, which might be
    # an HTTP or HTTPS server.

    nsf::proc upstream {
        when
        -target
        {-timeout 10.0}
        {-sendtimeout 0.0}
        {-receivetimeout 0.5}
        {-validation_callback ""}
        {-regsubs:0..n ""}
        {-exception_callback "::revproxy::exception"}
        {-url_rewrite_callback "::revproxy::rewrite_url"}
        {-backend_reply_callback ""}
        {-backendconnection ""}
    } {

        if {$backendconnection eq ""} {
            set backendconnection \
                [ns_config ns/server/[ns_info server]/module/revproxy backendconnection ns_connchan]
        }
        if {[llength $target] > 0} {
            set md5 [ns_md5 $target]
            set count [nsv_incr module:revproxy:proxyset $md5]
            # log notice "===== upstream [ns_info server] <$target> count $count"
            set target [lindex $target [expr {$count % [llength $target]}]]
        }
        nsv_incr module:revproxy:target $target
        log notice "===== upstream [ns_info server] ===== [ns_conn method] $target"

        if {[ns_set iget [ns_conn headers] Upgrade] eq "websocket"} {
            #
            # We received a WebSocket upgrade response from the
            # server. WebSocket use long open connections, we can
            # support these only via "ns_connchan", since "ns_http"
            # gets the data in one sweep. So force the ns_connchan
            # handler if necessary.
            #
            log notice "WebSocket upgrade, forcing long timeouts"
            if {$backendconnection ne "ns_connchan"} {
                log notice "switch backend connection from '$backendconnection'" \
                    "to 'ns_connchan', since a WebSocket upgrade was received."
                set backendconnection ns_connchan
            }
            #
            # WebSockets are long running requests, where no data
            # might be received for a long time. Therefore, we force a
            # long timeout.
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
                     -query [ns_conn query]]
        log notice "===== submit via ${backendconnection}, [ns_conn method] $url"

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
        # Get header fields from request, add X-Forwarded-For,
        # X-Forwarded-Proto, and X-SSL-Request (if appropriate).
        #
        set queryHeaders [ns_conn headers]

        set XForwardedFor [split [ns_set iget $queryHeaders "X-Forwarded-For" ""] " ,"]
        set XForwardedFor [lmap e $XForwardedFor {if {$e eq ""} continue}]
        lappend XForwardedFor [ns_conn peeraddr]
        ns_set update $queryHeaders X-Forwarded-For [join $XForwardedFor ","]

        set proto [dict get [ns_parseurl $url] proto]
        ns_set update $queryHeaders X-Forwarded-Proto $proto
        if {$proto eq "https"} {
            ns_set update $queryHeaders X-SSL-Request 1
        }

        #
        # Finally, start the transmission to the backend via the
        # configured means.
        #
        return [::revproxy::${backendconnection}::upstream \
                    -url $url \
                    -timeout $timeout \
                    -sendtimeout $sendtimeout \
                    -receivetimeout $receivetimeout \
                    -validation_callback $validation_callback \
                    -exception_callback $exception_callback \
                    -backend_reply_callback $backend_reply_callback \
                   ]
    }

    nsf::proc upstream_send_failed {
        {-status 503}
        -errorMsg
        -frontendChan
        -backendChan
        -url
        {-exception_callback ""}
    } {
        ns_log error "revproxy::upstream: error during establishing connections to $url: $errorMsg"
        if {$exception_callback ne ""} {
            {*}$exception_callback -status $status -error $errorMsg -url $url
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
    } {
        if {$msg eq ""} {
            ns_log warning "Opening connection to backend [ns_quotehtml $url] failed with status $status"
            set msg "Backend error: [ns_quotehtml $error]"
        }
        ns_returnerror $status $msg
    }

    #
    # Default rewrite_url handler for composing upstream target URL
    # based on the target (containing at typically the protocol and
    # location), the incoming URL and the actual query parameter form
    # the incoming URL.
    #
    nsf::proc rewrite_url { -target -url {-query ""}} {
        set url [string trimright $target /]/[string trimleft $url /.]
        if {$query ne ""} {append url ?$query}
        return $url
    }


    #
    # Simple logger for error log, evaluating the ::revproxy::verbose
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
    set filters [ns_config ns/server/[ns_info server]/module/revproxy filters]
    if {$filters ne ""} {
        ns_log notice "==== revproxy registers filters\n$filters"
        eval $filters
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
