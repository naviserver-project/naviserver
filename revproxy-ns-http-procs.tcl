#-------------------------------------------------------------------------------
# Implementation of the reversse proxy backend connection vis "ns_http"
#-------------------------------------------------------------------------------

namespace eval ::revproxy {}
namespace eval ::revproxy::ns_http {

    nsf::proc upstream {
        -url
        {-timeout 15.0}
        {-sendtimeout 0.0}
        {-receivetimeout 0.5}
        {-validation_callback ""}
        {-exception_callback "::revproxy::exception"}
        {-backend_reply_callback ""}
    } {
        #
        # Now perform the transmission... but before this, call the
        # validation callback on the final result. The callback
        # invocation is in the delivery-method specific code, since
        # the delivery method might add headers etc.
        #
        if {$validation_callback ne ""} {
            {*}$validation_callback -url $url
        }
        
        set queryHeaders [ns_conn headers]        
        set binary false
        set extraArgs {}

        switch [ns_conn method] {
            PUT -
            POST {
                set contentType [ns_set iget $queryHeaders content-type]
                if {$contentType ne ""
                    && [ns_encodingfortype $contentType] eq "binary"} {
                    set binary true
                }
                set contentfile [ns_conn contentfile]
                if {$contentfile ne ""} {
                    lappend extraArgs -body_file $contentfile
                } elseif {$binary} {
                    lappend extraArgs -body [ns_conn content -binary]
                } else {
                            lappend extraArgs -body [ns_conn content]
                }
            }
            GET {}
        }
        if {$binary} {
            lappend extraArgs -binary
        }

        #log notice "final request headers passed to ns_http"
        #ns_set print $queryHeaders
        
        if {[catch {
            try {
                ns_http run \
                    -keep_host_header \
                    -spoolsize 100kB \
                    -method [ns_conn method] \
                    -headers $queryHeaders \
                    -expire $timeout \
                    {*}$extraArgs \
                    $url

            } trap {NS_TIMEOUT} {errorMsg} {
                log notice "TIMEOUT during send to $url ($errorMsg) "
                ns_returnerror 504 "Gateway Timeout"

            } on ok {r} {
                set replyHeaders [dict get $r headers]
                set outputheaders [ns_conn outputheaders]
                #ns_log notice "RESULT of query: $r"
                #ns_log notice "... reply headers  <$replyHeaders> <[ns_set array $replyHeaders]>"

                if {$backend_reply_callback ne ""} {
                    {*}$backend_reply_callback -url $url -replyHeaders $replyHeaders -status $status
                }

                foreach {key value} [ns_set array [dict get $r headers]] {
                    if {[string tolower $key] ni {
                        connection date server
                        content-length content-encoding
                    }} {
                        ns_set put $outputheaders $key $value
                        log notice "adding to output headers: <$key> <$value>"
                    }
                }
                #ns_set update $outputheaders Connection close
                #
                # Pass the status code
                #
                log notice "backend status code [dict get $r status]"
                ns_headers [dict get $r status]

                #
                # Get the content either as a string or from a spool
                # file (to avoid memory bloats on huge files).
                #
                if {[dict exists $r body]} {
                    log notice "submit string (length [string length [dict get $r body]])"
                    ns_writer submit [dict get $r body]
                } elseif {[dict exists $r file]} {
                    log notice "submit file <[dict get $r file]>"
                    ns_writer submitfile [dict get $r file]
                } else {
                    error "REVERSE PROXY <$url>: invalid return dict with keys <[dict keys $r]"
                }
            }

        } errorMsg]} {
            log warning "request to URL <$url> returned //$errorMsg//"
            ::revproxy::upstream_send_failed \
                -errorMsg $errorMsg \
                -url $url \
                -exception_callback $exception_callback
        }
        return filter_return
    }

    interp alias {} log {} ::revproxy::log
}

#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
