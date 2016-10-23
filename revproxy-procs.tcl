package require nsf

#
# This file can be installed e.g. as a tcl module in /ns/tcl/revproxy/revproxy-procs.tcl
# and registered as a tcl module in the config file.
#

#ns_register_filter postauth GET  /shiny/* revproxy::upstream -target https://learn.wu.ac.at/
#ns_register_filter postauth POST /shiny/* revproxy::upstream -target https://learn.wu.ac.at/

namespace eval ::revproxy {

    set version 0.1

    #
    # Upstream handler (deliver request from an upstream server)
    #
    # Serve the requested file from an upstream server, which might be
    # an http or https server. NaviServer acts as a reverse proxy
    # server.  The upstream function works incrementally and functions
    # as well for WebSockets (including secure WebSockets).  Note that
    # we can specify for every filter registration different parameters
    # (e.g. different timeouts).
    #
   
    nsf::proc upstream { what
	-target
	{-timeout 10:0}
	{-validation_callback ""}
	{-exception_callback "::revproxy::exception"}
    } {
	#
	# Assemble URL
	#
	set url $target[ns_conn url]
	set query [ns_conn query]
	if {$query ne ""} {append url ?$query}

	#
	# Get header fields from request, add X-Forwarded-For.
	#
	# We might update/add more headers fields here
	#
	set queryHeaders [ns_conn headers]
	ns_set update $queryHeaders X-Forwarded-For [ns_conn peeraddr]

	if {$validation_callback ne ""} {
	    {*}$validation_callback -url $url
	}

	if {[catch {
	    #
	    # Open backend channel, get frontend channel and connect these.
	    #
	    set backendChan  [ns_connchan open -method [ns_conn method] -headers $queryHeaders $url]
	    set frontendChan [ns_connchan detach]
	    
	    ns_connchan callback $backendChan  [list ::revproxy::spool $backendChan $frontendChan $url 0] rex
	    ns_connchan callback $frontendChan [list ::revproxy::spool $frontendChan $backendChan client 0] rex
	    
	} errorMsg]} {
	    ns_log error "revproxy::upstream: error during establishing connections to $url: $errorMsg"
	    if {$exception_callback ne ""} {
		{*}$exception_callback -error $errorMsg -url $url
	    }
	    foreach chan {frontendChan backendChan} {
		if {[info exists $chan]} {
		    ns_connchan close [set $chan]
		}
	    }
	}
	return filter_return
    }
    
    #
    # Spool data from $to to $to. The url and arg might be used
    # for debugging, when is the one-character reason code for
    # calling this function.
    #
    # When this function returns 0, this channel end will be
    # automatically closed.
    #
    nsf::proc spool { from to url arg when } {
	set msg [ns_connchan read $from]
	if {$msg eq ""} {
	    #ns_log notice "... auto closing $from $url ($ws)"
	    set result 0
	    ns_connchan close $to
	} else {
	    ns_connchan write $to $msg
	    set result 1
	}
	return $result
    }

    #
    # Sample exception handler
    #
    nsf::proc exception { -error -url } {
	ns_returnerror 503 \
	    "Error during opening connection to backend [ns_quotehtml $url] failed. \
	     <br>Error message: [ns_quotehtml $error]"
    }
    
    #
    # Get configured urls
    #
    set filters [ns_config ns/server/[ns_info server]/module/revproxy filters]
    if {$filters ne ""} {
	ns_log notice "==== revproxy registers filters\n$filters"
	eval $filters
    }
   
    ns_log notice "==== revproxy module version $version loaded"
}
