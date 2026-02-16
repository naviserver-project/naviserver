#---------------------------------------------------------------------
# Access log -- core module "nslog"
#---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    ns_param nslog   nslog
}
ns_section ns/server/$server/module/nslog {
    #------------------------------------------------------------------
    # General parameters for access.log
    #------------------------------------------------------------------
    # ns_param driver "http*"  ;# access log lists only entries for matching drivers
    #                           # important, when using multiple access logs per server
    ns_param file access.log   ;# default: access.log
    # ns_param maxbuffer 100   ;# default: 0; number of log entries buffered
                               ;# in memory before being flushed to disk

    #------------------------------------------------------------------
    # Control what to log
    #------------------------------------------------------------------
    # Suppress query string in the logged URL.
    # ns_param suppressquery  true    ;# default: false

    # Include the total time spent servicing the request.
    # ns_param logreqtime     true    ;# default: false

    # Include high-resolution start time and partial timings for each
    # request (accept, queue, filter, run, etc.).
    ns_param logpartialtimes  true    ;# default: false

    # Include the thread name in each log line, useful for correlating
    # access.log entries with system log (nsd.log)
    ns_param logthreadname    true    ;# default: false

    #------------------------------------------------------------------
    # Time formatting options.
    #------------------------------------------------------------------
    # ns_param formattedtime   true    ;# default: true; formatted timestamps vs unix time
    # ns_param logcombined     true    ;# default: true; NSCA Combined format (referer, user-agent)

    #------------------------------------------------------------------
    # Reverse proxy handling.
    #------------------------------------------------------------------
    # When running behind a reverse proxy, enable detection of
    # X-Forwarded-For / proxy headers so that the client IP is logged
    # correctly. Deprecated, replaced by "ns/parameters/reversproxymode"
    # ns_param checkforproxy    $reverseproxymode  ;# default: false

    #------------------------------------------------------------------
    # Address masking (privacy / GDPR)
    #------------------------------------------------------------------
    # Mask IP addresses in the log file (similar to "anonip" anonymizer).
    # When enabled, the mask settings below are applied to logged client
    # addresses.
    ns_param masklogaddr      true               ;# default: false

    # Network masks for IPv4 and IPv6 addresses.
    ns_param maskipv4         255.255.255.0      ;# example: mask last octet
    ns_param maskipv6         ff:ff:ff:ff::      ;# example: mask lower 64 bits

    #------------------------------------------------------------------
    # Extended headers (additional log fields)
    #------------------------------------------------------------------
    # You can add extra fields to access.log by specifying a Tcl list of
    # request header names in "extendedheaders".
    #
    # Example: log the OpenACS x-user-id header only when the site is
    # configured to include user_ids in logs.
    if {[ns_config "ns/server/$server/acs" LogIncludeUserId 0]} {
        ns_param extendedheaders "x-user-id"
    }

    #------------------------------------------------------------------
    # Log rotation
    #------------------------------------------------------------------
    # ns_param maxbackup   100     ;# default: 10; max number of rotated log files
    # ns_param rolllog     true    ;# default: true; rotate logs automatically
    # ns_param rollhour    0       ;# default: 0; hour of day to roll (0--23)
    # ns_param rollonsignal true   ;# default: false; rotate log on SIGHUP

    # Suffix appended to the log filename when rolling.
    ns_param rollfmt      %Y-%m-%d
}

