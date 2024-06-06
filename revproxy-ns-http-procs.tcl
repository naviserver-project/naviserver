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
            default {}
        }
        if {$binary} {
            lappend extraArgs -binary
        }

        #log notice "final request headers passed to ns_http"
        #ns_set print $queryHeaders

        try {
            ns_http run \
                {*}[expr {[ns_info version]>=5 ?  "-partialresults" : ""}] \
                -keep_host_header \
                -spoolsize 100kB \
                -method [ns_conn method] \
                -headers $queryHeaders \
                -expire $timeout \
                {*}$extraArgs \
                $url

        } trap {NS_TIMEOUT} {errorMsg} {
            log notice "TIMEOUT during send to $url ($errorMsg) "
            ::revproxy::upstream_send_failed \
                -status 504 \
                -errorMsg $errorMsg \
                -url $url \
                -exception_callback $exception_callback

        } on ok {r} {
            set replyHeaders [dict get $r headers]
            set outputHeaders [ns_conn outputheaders]
            #ns_log notice "RESULT of query: $r"
            #ns_log notice "... reply headers  <$replyHeaders> <[ns_set array $replyHeaders]>"

            if {$backend_reply_callback ne ""} {
                {*}$backend_reply_callback -url $url -replyHeaders $replyHeaders -status $status
            }

            if {[dict exists $r error]} {
                #
                # This request was sent with -partialresults enabled
                #
                set errorMsg [dict get $r error]
                log notice "RESULT contains error: '$errorMsg' /$::errorCode/"
                if {[string match *timeout* $errorMsg]} {
                    if {[ns_set iget $replyHeaders content-length ""] eq ""} {
                        #
                        # We have a timeout, and not content
                        # length. This is potentially a streaming
                        # HTML request, which cannot be handled
                        # currently by ns_http. Diagnose this
                        # condition in the log file, but still
                        # raise an error.
                        #
                        ns_log error "revproxy: streaming HTML attempt detected on ns_http for URL $url." \
                            "Please switch for this request to the ns_connchan interface!"
                    }

                    log notice "TIMEOUT during send to $url ($errorMsg) "
                    ::revproxy::upstream_send_failed \
                        -status 504 \
                        -errorMsg $errorMsg \
                        -url $url \
                        -exception_callback $exception_callback
                } else {
                    ::revproxy::upstream_send_failed \
                        -errorMsg $errorMsg \
                        -url $url \
                        -exception_callback $exception_callback
                }
                return filter_return
            }
            foreach {key value} [ns_set array [dict get $r headers]] {
                if {[string tolower $key] ni {
                    connection date server
                    content-length content-encoding
                }} {
                    ns_set put $outputHeaders $key $value
                    log notice "adding to output headers: <$key> <$value>"
                }
            }
            #ns_set update $outputHeaders Connection close
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
        } on error {errorMsg} {
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
