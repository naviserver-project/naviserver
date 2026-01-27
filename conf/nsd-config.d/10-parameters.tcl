########################################################################
#
# Section 1 -- Global NaviServer parameters (ns/parameters)
#
########################################################################

ns_section ns/parameters {

    #
    # General server settings
    #
    ns_param    home                $home
    ns_param    logdir              $logdir
    #ns_param    bindir             bin
    ns_param    tcllibrary          tcl
    #ns_param    pidfile             ${home}/logs/nsd.pid

    # Parameter for controlling caching via ns_cache. Potential values
    # are "full" or "none", future versions might allow as well
    # "cluster".  The value of "none" makes ns_cache operations to
    # no-ops, this is a very conservative value for clusters.
    #
    # ns_param  cachingmode      "none"      ;# default: "full"

    #ns_param    progressminsize     1MB      ;# default: 0
    #ns_param    listenbacklog       256      ;# default: 32; backlog for ns_socket commands

    # Reject output operations on already closed or detached connections (e.g. subsequent ns_return statements)
    #ns_param    rejectalreadyclosedconn false;# default: true

    #
    # Tcl settings
    #
    #ns_param    tclinitlock         true     ;# default: false
    #ns_param    concurrentinterpcreate false ;# default: true
    #ns_param    mutexlocktrace      true     ;# default false; print durations of long mutex calls to stderr

    #
    # Log settings (systemlog aka nsd.log, former error.log)
    #
    #ns_param    systemlog           nsd.log  ;# default: nsd.log
    #ns_param    logdebug            true     ;# default: false
    #ns_param    logrollonsignal     false    ;# default: true
    #ns_param    logrollfmt          %Y-%m-%d ;# format appended to log filename
    #ns_param    logsec              false    ;# add timestamps in second resolution (default: true)
    #ns_param    logusec             true     ;# add timestamps in microsecond (usec) resolution (default: false)
    #ns_param    logusecdiff         true     ;# add timestamp diffs since in microsecond (usec) resolution (default: false)
    #ns_param    logthread           false    ;# add thread-info the log file lines (default: true)
    #ns_param    sanitizelogfiles    1        ;# default: 2; 0: none, 1: full, 2: human-friendly, 3: 2 with tab expansion
    #ns_param    logdeduplicate      true     ;# default: false; collapse multiple identical log lines from a thread

    #
    # Encoding settings
    #
    # ns_param   OutputCharset       utf-8
    # ns_param   URLCharset          utf-8
    ns_param     formfallbackcharset iso8859-1 ;# retry with this charset in case of failures

    #
    # Jobs setting
    #
    ns_param     jobsperthread       1000     ;# default: 0
    #ns_param    jobtimeout          0s       ;# default: 5m
    ns_param     joblogminduration   100s     ;# default: 1s
    ns_param     schedsperthread     10       ;# number of jobs before restart; default: 0 (no auto restart)
    #ns_param    schedlogminduration 2s       ;# print warnings when scheduled job takes longer than that

    #ns_param    dbcloseonexit       off      ;# default: off; from nsdb

    #
    # Configure the number of task threads for ns_http
    #
    # ns_param   nshttptaskthreads   2        ;# default: 1; number of task threads for ns_http

    #
    # Configure SMTP module
    #
    ns_param     smtphost            "localhost"
    ns_param     smtpport            25
    ns_param     smtptimeout         60
    ns_param     smtplogmode         false
    ns_param     smtpmsgid           false
    ns_param     smtpmsgidhostname   ""
    ns_param     smtpencodingmode    false
    ns_param     smtpencoding        "utf-8"
    ns_param     smtpauthmode        ""
    ns_param     smtpauthuser        ""
    ns_param     smtpauthpassword    ""
}

#
# When running behind a reverse proxy, use the following parameters
#
ns_section ns/parameters/reverseproxymode {
    #
    # Is the server running behind a reverse proxy server?
    #
    ns_param enabled $reverseproxymode
    #
    # When defining "trustedservers", the x-forwarded-for header field
    # is only accepted in requests received from one of the specified
    # servers. The list of servers can be provided by using IP
    # addresses or CIDR masks. Additionally, the processing mode of
    # the contents of the x-forwarded-for contents switches to
    # right-to-left, skipping trusted servers. So, the danger of
    # obtaining spoofed addresses can be reduced.
    #
    ns_param trustedservers $trustedservers
    #
    # Optionally, non-public entries in the content of x-forwarded-for
    # can be ignored. These are not useful for e.g. geo-location
    # analysis.
    #
    #ns_param skipnonpublic  false
}
