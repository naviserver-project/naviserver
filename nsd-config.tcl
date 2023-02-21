########################################################################
# Sample configuration file for NaviServer
########################################################################

# All default variables in "defaultConfig" can be overloaded by:
#
# 1) Setting these variables explicitly in this file after
#    "ns_configure_variables" (highest precedence)
#
# 2) Setting these variables as environment variables with the "nsd_"
#    prefix (suitable for e.g. docker setups).  The lookup for
#    environment variables happens in "ns_configure_variables".
#
# 3) Alter/override the variables in the "defaultConfig"
#    (lowest precedence)
#
# Some comments:
#   "ipaddress":
#       specify an IPv4 or IPv6 address, or a blank separated
#       list of such addresses
#   "httpport":
#       might be as well a list of ports, when listening on
#       multiple ports
#   "nscpport":
#       when non-empty, load the nscp module and listen
#       on the specified port
#   "home":
#       the root directory, containng the subdirecturies
#       "pages", "logs", "lib", "bin", "tcl", ....
#
dict set defaultConfig ipaddress 0.0.0.0
dict set defaultConfig httpport 8080
dict set defaultConfig nscpport ""
dict set defaultConfig home [file dirname [file dirname [info nameofexecutable]]]

#
# For all potential variables defined by the dict "defaultConfig",
# allow environment variables such as "oacs_httpport" or
# "oacs_ipaddress" to override local values.
#
source [file dirname [ns_info nsd]]/../tcl/init.tcl
ns_configure_variables "nsd_" $defaultConfig

########################################################################
# Global settings (for all servers)
########################################################################

ns_section ns/parameters {

    #
    # General server settings
    #
    ns_param    home                $home
    ns_param    tcllibrary          tcl
    #ns_param   pidfile             ${home}/logs/nsd.pid

    #ns_param   progressminsize     1MB      ;# default: 0
    #ns_param   listenbacklog       256      ;# default: 32; backlog for ns_socket commands

    # Reject output operations on already closed connections (e.g. subsequent ns_return statements)
    #ns_param   rejectalreadyclosedconn false;# default: true
    #ns_param   reverseproxymode    true     ;# running behind a reverse proxy server? (default: false

    #
    # Tcl settings
    #
    #ns_param   tclinitlock         true     ;# default: false
    #ns_param   concurrentinterpcreate false ;# default: true
    #ns_param   mutexlocktrace      true     ;# default false; print durations of long mutex calls to stderr

    #
    # Log settings (systemlog aka error.log)
    #
    ns_param    serverlog           error.log
    #ns_param   logdebug            true     ;# default: false
    #ns_param   logroll             false    ;# default: true
    #ns_param	logrollfmt          %Y-%m-%d ;# format appended to log filename
    #ns_param   logsec              false    ;# add timestamps in second resolution (default: true)
    #ns_param   logusec             true     ;# add timestamps in microsecond (usec) resolution (default: false)
    #ns_param   logusecdiff         true     ;# add timestamp diffs since in microsecond (usec) resolution (default: false)
    #ns_param   logthread           false    ;# add thread-info the log file lines (default: true)
    #ns_param   sanitizelogfiles    1        ;# default: 2; 0: none, 1: full, 2: human-friendly

    #
    # Encoding settings
    #
    # ns_param	OutputCharset	utf-8
    # ns_param	URLCharset	utf-8
    ns_param formfallbackcharset iso8859-1 ;# retry with this charset in case of failures

    #
    # Jobs setting
    #
    ns_param    jobsperthread       1000     ;# default: 0
    #ns_param   jobtimeout          0s       ;# default: 5m
    ns_param	joblogminduration   100s     ;# default: 1s
    ns_param    schedsperthread     10       ;# default: 0
    #ns_param	schedlogminduration 2s       ;# print warnings when scheduled job takes longer than that

    #ns_param   dbcloseonexit       off      ;# default: off; from nsdb

    # configure SMTP module
    ns_param    smtphost            "localhost"
    ns_param    smtpport            25
    ns_param    smtptimeout         60
    ns_param    smtplogmode         false
    ns_param    smtpmsgid           false
    ns_param    smtpmsgidhostname   ""
    ns_param    smtpencodingmode    false
    ns_param    smtpencoding        "utf-8"
    ns_param    smtpauthmode        ""
    ns_param    smtpauthuser        ""
    ns_param    smtpauthpassword    ""
}

ns_section ns/threads {
    ns_param    stacksize           512kB
}

ns_section ns/mimetypes {
    ns_param    default             text/plain
    ns_param    noextension         text/plain
}

ns_section ns/fastpath {
    #ns_param   cache               false      ;# default: false
    #ns_param   cachemaxsize        10MB       ;# default: 10MB
    #ns_param   cachemaxentry       8kB        ;# default: 8kB
    #ns_param   mmap                false      ;# default: false
    ns_param    gzip_static         true       ;# check for static gzip; default: false
    ns_param    gzip_refresh        true       ;# refresh stale .gz files on the fly using ::ns_gzipfile
    ns_param    gzip_cmd            "/usr/bin/gzip -9"  ;# use for re-compressing
    ns_param    brotli_static       true       ;# check for static brotli files; default: false
    ns_param    brotli_refresh      true       ;# refresh stale .br files on the fly using ::ns_brotlifile
    ns_param    brotli_cmd          "/usr/bin/brotli -f -Z"  ;# use for re-compressing
    #ns_param   brotli_cmd          "/opt/local/bin/brotli -f -Z"  ;# use for re-compressing (macOS + ports)
}

ns_section ns/servers {
    ns_param default "My First NaviServer Instance"
}

#
# Global modules (for all servers)
#
ns_section ns/modules {
    ns_param    nssock              nssock
}

ns_section ns/module/nssock {
    ns_param    defaultserver            default
    ns_param    port                     $httpport
    ns_param    address                  $ipaddress   ;# Space separated list of IP addresses
    #ns_param    hostname                [ns_info hostname]
    ns_param    maxinput                 10MB         ;# default: 1MB, maximum size for inputs (uploads)
    #ns_param   readahead                1MB          ;# default: 16384; size of readahead for requests
    ns_param    backlog                  1024         ;# default: 256; backlog for listen operations
    ns_param    acceptsize               10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
    ns_param    closewait                0s           ;# default: 2s; timeout for close on socket
    #ns_param   maxqueuesize             1024         ;# default: 1024; maximum size of the queue
    #ns_param   keepwait                 5s           ;# 5s, timeout for keep-alive
    ns_param    keepalivemaxuploadsize   0.5MB        ;# 0, don't allow keep-alive for upload content larger than this
    ns_param    keepalivemaxdownloadsize 1MB          ;# 0, don't allow keep-alive for download content larger than this
    #
    # TCP tuning
    #
    #ns_param  nodelay         false   ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
    #
    # Spooling Threads
    #
    #ns_param   spoolerthreads		1	;# default: 0; number of upload spooler threads
    ns_param    maxupload		1MB     ;# default: 0, when specified, spool uploads larger than this value to a temp file
    ns_param    writerthreads		1	;# default: 0, number of writer threads
    #ns_param   writersize		1MB	;# default: 1MB, use writer threads for files larger than this value
    #ns_param   writerbufsize		8kB	;# default: 8kB, buffer size for writer threads
    #ns_param   driverthreads           2	;# default: 1, number of driver threads (requires support of SO_REUSEPORT)

    # Extra driver-specific response header fields (probably for nsssl)
    #ns_param   extraheaders  {Strict-Transport-Security "max-age=31536000; includeSubDomains"}
}

#
# The following section defines, which hostnames map to which
# server. In our case for example, the host "localhost" is mapped to
# the nsd server named "default".
#
ns_section ns/module/nssock/servers {
    ns_param default    localhost
    ns_param default    [ns_info hostname]
}

########################################################################
#  Settings for the "default" server
########################################################################

ns_section ns/server/default {
    ns_param    enabletclpages      true  ;# default: false
    #ns_param   filterrwlocks       false ;# default: true
    ns_param    checkmodifiedsince  false ;# default: true, check modified-since before returning files from cache. Disable for speedup
    ns_param    connsperthread      1000  ;# default: 0; number of connections (requests) handled per thread
    ns_param    minthreads          5     ;# default: 1; minimal number of connection threads
    ns_param    maxthreads          100   ;# default: 10; maximal number of connection threads
    #ns_param    maxconnections     100   ;# default: 100; number of allocated connection structures
    ns_param    rejectoverrun       true  ;# default: false; send 503 when thread pool queue overruns
    #ns_param   threadtimeout       2m    ;# default: 2m; timeout for idle connection threads
    #ns_param   concurrentcreatethreshold 100 ;# default: 80; perform concurrent creates when queue is fully beyond this percentage
    ;# 100 is a conservative value, disabling concurrent creates
    #ns_param    connectionratelimit 200  ;# 0; limit rate per connection to this amount (KB/s); 0 means unlimited
    #ns_param    poolratelimit       200  ;# 0; limit rate for pool to this amount (KB/s); 0 means unlimited

    # Extra server-specific response header fields
    #ns_param   extraheaders  {Referrer-Policy "strict-origin"}
}

ns_section ns/server/default/modules {
    if {$nscpport ne ""} {ns_param nscp nscp}
    ns_param    nslog               nslog
    ns_param    nscgi               nscgi
}

ns_section ns/server/default/fastpath {
    #ns_param   pagedir             pages
    #ns_param   serverdir           ""
    #ns_param   directoryfile       "index.adp index.tcl index.html index.htm"
    #ns_param   directoryproc       _ns_dirlist
    ns_param    directorylisting    fancy    ;# default: simple
    #ns_param   directoryadp       dir.adp
}

ns_section ns/server/default/vhost {
    #ns_param    enabled             false
    #ns_param    hostprefix          ""
    #ns_param    hosthashlevel       0
    #ns_param    stripport           true
    #ns_param    stripwww            true
}

ns_section ns/server/default/adp {
    ns_param    map                 "/*.adp"
    #ns_param   enableexpire        false    ;# default: false; set "Expires: now" on all ADP's
    #ns_param   enabledebug         true     ;# default: false
    #ns_param   enabletclpages      true     ;# default: false
    #ns_param   singlescript        false    ;# default: false; collapse Tcl blocks to a single Tcl script
    #ns_param   cache               false    ;# default: false; enable ADP caching
    #ns_param   cachesize           5MB
    #ns_param   bufsize             1MB
}

ns_section ns/server/default/tcl {
    ns_param    nsvbuckets          16       ;# default: 8
    ns_param    nsvrwlocks          false    ;# default: true
    ns_param    library             modules/tcl
    #
    # Example for initcmds (to be executed, when this server is fully initialized).
    #
    #ns_param    initcmds {
    #    ns_log notice "=== Hello World === server: [ns_info server] running"
    #}
}

ns_section ns/server/default/module/nscgi {
    ns_param    map                 "GET  /cgi-bin $home/cgi-bin"
    ns_param    map                 "POST /cgi-bin $home/cgi-bin"
    ns_param    interps              CGIinterps
    #ns_param   allowstaticresources true    ;# default false; serve static resources from cgi directories
}

ns_section ns/interps/CGIinterps {
    ns_param	.pl                 "/opt/local/bin/perl"
    ns_param	.sh                 "/bin/bash"
}

ns_section ns/server/default/module/nslog {
    #ns_param   file                access.log
    #ns_param   rolllog             true     ;# default: true; should server log files automatically
    #ns_param   rollonsignal        false    ;# default: false; perform roll on a sighup
    #ns_param   rollhour            0        ;# default: 0; specify at which hour to roll
    ns_param    maxbackup           7        ;# default: 10; max number of backup log files
    #ns_param   rollfmt             %Y-%m-%d-%H:%M	;# format appended to log filename
    #ns_param   logpartialtimes     true     ;# default: false
    #ns_param   logreqtime          true     ;# default: false; include time to service the request
    ns_param    logthreadname       true     ;# default: false; include thread name for linking with error.log

    ns_param	masklogaddr         true    ;# false, mask IP address in log file for GDPR (like anonip IP anonymizer)
    ns_param	maskipv4            255.255.255.0  ;# mask for IPv4 addresses
    ns_param	maskipv6            ff:ff:ff:ff::  ;# mask for IPv6 addresses
}

ns_section ns/server/default/module/nscp {
    ns_param   port     $nscpport
    #ns_param   address  0.0.0.0
}

ns_section ns/server/default/module/nscp/users {
    ns_param user "::"
}

set ::env(RANDFILE) $home/.rnd
set ::env(HOME) $home
set ::env(LANG) en_US.UTF-8

#
# For debugging, you might activate one of the following flags
#
#ns_logctl severity Debug(ns:driver) on
#ns_logctl severity Debug(request) on
#ns_logctl severity Debug(task) on
#ns_logctl severity Debug(sql) on
#ns_logctl severity Debug(nsset) on
