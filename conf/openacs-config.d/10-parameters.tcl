######################################################################
# Section 1 -- Global NaviServer parameters (ns/parameters)
######################################################################
# Global NaviServer parameters
#---------------------------------------------------------------------

ns_section ns/parameters {
    #------------------------------------------------------------------
    # Core paths and process options
    #------------------------------------------------------------------

    ns_param home     $homedir
    ns_param logdir   $logdir
    ns_param pidfile  nsd.pid
    ns_param  debug    $debug

    # Optional directory for temporary files. If not specified, the
    # environment variable TMPDIR is used. If that is not set either,
    # a system-specific compile-time constant is used (macro
    # P_tmpdir). Should be only necessary for Windows base systems.
    #
    # ns_param tmpdir   c:/tmp

    #------------------------------------------------------------------
    # Caching and upload behaviour
    #------------------------------------------------------------------

    # Parameter for controlling caching via ns_cache.
    #   full    -- normal operation (default)
    #   none    -- make all ns_cache operations no-ops
    #   cluster -- reserved for future clustered setups
    #
    # Using "none" is the most conservative setting, especially in
    # clusters or complex deployments.
    ns_param cachingmode $cachingmode       ;# default: "full"

    # Minimum size for enabling optional upload progress bars.
    # ns_param progressminsize 1MB          ;# default: 0

    # Timeout for shutdown: time to let existing connections and
    # background jobs finish gracefully before forcing exit.
    #
    # ns_param shutdowntimeout 20s          ;# default: 20s


    #------------------------------------------------------------------
    # Incoming and outgoing connections
    #------------------------------------------------------------------

    # Configuration of incoming connections (global defaults).
    #
    # ns_param listenbacklog   256          ;# default: 32; backlog for ns_socket
    #                                        # and default for drivers; can be
    #                                        # overridden per driver via "backlog".
    #
    # ns_param sockacceptlog   3            ;# default: 4; log when a single accept
    #                                        # call receives more than this number
    #                                        # of sockets; can be overridden per
    #                                        # driver with the same parameter.

    # Configuration of outgoing HTTP requests via ns_http.
    #
    # ns_param nshttptaskthreads 2          ;# default: 1; number of task threads
    #                                        # used when ns_http runs detached.
    #
    # ns_param autoSNI           false      ;# default: true; enable SNI
    #                                        # (Server Name Indication in TLS)
    #

    #------------------------------------------------------------------
    # HTTP request handling safety (incoming requests)
    #------------------------------------------------------------------
    # Control how the server behaves when application code attempts to
    # send output on a connection that is already closed or detached
    # (for example, a second ns_return on the same incoming request).
    #
    # true  (default) -- reject such operations and log an error; avoids
    #                    undefined behaviour and confusing partial output.
    # false           -- allow them (legacy behaviour; not recommended).
    #
    # ns_param rejectalreadyclosedconn false ;# default: true

    #------------------------------------------------------------------
    # System log (nsd.log)
    #------------------------------------------------------------------

    # Log filename (system log).
    # ns_param systemlog    nsd.log         ;# default: nsd.log

    # Logfile rotation.
    ns_param logrollonsignal on            ;# default: off; rotate log on SIGHUP
    ns_param logmaxbackup   100            ;# default: 10; number of rotated logs to keep
    ns_param logrollfmt     %Y-%m-%d       ;# suffix appended to logfile name when rolled

    # Format of log entries in the system log.
    #
    # ns_param logsec         false         ;# default: true; timestamps in seconds
    # ns_param logusec        true          ;# default: false; timestamps in microseconds
    # ns_param logusecdiff    true          ;# default: false; deltas between log entries (usec)
    # ns_param  logrelative   true          ;# default: false; start timestamps from zero
    # ns_param logthread      false         ;# default: true; include thread id in log lines
    ns_param logcolorize     true           ;# default: false; ANSI-colored log output
    ns_param logprefixcolor  green          ;# black, red, green, yellow, blue, magenta, cyan,
                                             # gray, default
    # ns_param logprefixintensity normal    ;# "bright" or "normal"


    # Which severities to log. Severity filters can be controlled more
    # flexibly at runtime via ns_logctl; the parameters below provide
    # static defaults, if set.
    #
    # ns_param logdebug          true       ;# default: false; debug messages
    # ns_param logdev            true       ;# default: false; development messages
    # ns_param lognotice         true       ;# default: true; informational messages
    # ns_param sanitizelogfiles  2          ;# default: 2; 0=none, 1=full, 2=human-friendly,
    #                                        # 3=like 2 plus tab expansion
    # ns_param logdeduplicate    true       ;# default: false; collapse repeated identical
    # log lines per thread

    # Write asynchronously to log files (access log, httpclient log,
    # and system log).
    ns_param asynclogwriter true          ;# default: false

    # Print durations of long mutex calls to stderr for debugging.
    # ns_param mutexlocktrace  true         ;# default: false


    #------------------------------------------------------------------
    # Mail and background jobs
    #------------------------------------------------------------------

    # Backward compatibility: used as a fallback for smtphost in
    # ns_sendmail; superseded by nssmtpd module.  ns_param mailhost
    # localhost

    # Background job and scheduler defaults (used by ns_job / ns_schedule_*).
    #
    # ns_param jobsperthread        0       ;# default: 0 (no auto restart); number of
    #                                        # ns_jobs per thread before thread restart
    # ns_param jobtimeout           5m      ;# default: 5m;
    # ns_param joblogminduration    1s      ;# default: 1s; log only jobs longer than this
    #

    ns_param schedsperthread      100       ;# default: 0 (no auto restart); number of
    #                                        # scheduled jobs per thread before restart;

    # ns_param schedlogminduration  2s      ;# default: 2s; log scheduled jobs that run longer than this


    #------------------------------------------------------------------
    # Tcl interaction
    #------------------------------------------------------------------

    # Allow concurrent creation of Tcl interpreters. Older Tcl
    # versions (up to 8.5) were known to crash when two threads
    # created interpreters at the same time. Serializing interpreter
    # creation helped in those cases. Starting with Tcl 8.6 the
    # default is "true".
    #
    # ns_param concurrentinterpcreate false ;# default: true

    # Enforce sequential thread initialization. This is usually not
    # desirable, but can help when investigating rare crashes or when
    # debugging with tools like valgrind.
    #
    # ns_param tclinitlock true             ;# default: false

    #------------------------------------------------------------------
    # Encoding settings
    #------------------------------------------------------------------

    # NaviServer's default charsets are all UTF-8. Although the
    # default charset is UTF-8, set the parameter "OutputCharset"
    # explicitly here, since otherwise OpenACS may use the charset
    # from [ad_conn charset], which is taken from the database and is
    # by default ISO-8859-1.
    ns_param OutputCharset utf-8
    # ns_param URLCharset   utf-8
    #
    # If UTF-8 parsing fails for form data, retry with the specified
    # fallback charset.
    #
    # ns_param formfallbackcharset iso8859-1

    #------------------------------------------------------------------
    # DNS configuration
    #------------------------------------------------------------------

    # ns_param dnscache        false       ;# default: true; enable DNS result caching
    # ns_param dnswaittimeout  5s          ;# default: 5s; timeout for DNS replies
    # ns_param dnscachetimeout 60m         ;# default: 60m (1h); time to keep entries in cache
    # ns_param dnscachemaxsize 500KB       ;# max in-memory size of DNS cache; default: 500KB

    #------------------------------------------------------------------
    # Legacy reverse proxy indicator
    #------------------------------------------------------------------

    # Running behind a reverse proxy? This setting is used by OpenACS
    # as well. It is considered legacy and kept for backward
    # compatibility; prefer configuring the section
    # "ns/parameters/reverseproxymode" instead (see below).
    ns_param reverseproxymode $reverseproxymode
}

#
# When running behind a reverse proxy, use the following parameters
#
ns_section ns/parameters/reverseproxymode {
    ns_param enabled        $reverseproxymode
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

#
# In case, a docker mapping is provided, source it to make it
# accessible during configuration. The mapping file is a Tcl script
# providing at least the Tcl dict ::docker::containerMapping
# containing the docker mapping. A dict key like "8080/tcp" (internal
# port) will return a dict containing the keys "host", "port" and
# "proto" (e.g. proto https host 192.168.1.192 port 58115).
#
if {[file exists /scripts/docker-dict.tcl]} {
    source /scripts/docker-dict.tcl
}




