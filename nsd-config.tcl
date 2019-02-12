########################################################################
# Sample config file for NaviServer
########################################################################

#
# Set the IP-address and port, on which the server listens:
#
set port 8080
set address "0.0.0.0"  ;# one might use as well for IPv6: set address ::

#
# Get the "home" directory from the currently executing binary.
# We could do alternatively:
#    set home /usr/local/ns
#
set home [file dirname [file dirname [info nameofexecutable]]]


########################################################################
# Global settings (for all servers)
########################################################################

ns_section "ns/parameters" {
    ns_param    home                $home
    ns_param    tcllibrary          tcl
    #ns_param   tclinitlock         true	       ;# default: false
    ns_param    serverlog           error.log
    #ns_param   pidfile             ${home}/logs/nsd.pid
    #ns_param   logdebug            true               ;# default: false
    #ns_param   logroll             false              ;# default: true
    #ns_param	logrollfmt	    %Y-%m-%d           ;# format appended to log file name
    #ns_param   dbcloseonexit       off                ;# default: off; from nsdb
    ns_param    jobsperthread       1000               ;# default: 0
    ns_param    jobtimeout          0                  ;# default: 300
    ns_param    schedsperthread     10                 ;# default: 0
    ns_param    progressminsize     1MB                ;# default: 0
    #ns_param   concurrentinterpcreate true            ;# default: false
    #ns_param   listenbacklog        256               ;# default: 32; backlog for ns_socket commands
    #ns_param   mutexlocktrace       true              ;# default false; print durations of long mutex calls to stderr

    # Reject output operations on already closed connections (e.g. subsequent ns_return statements)
    #ns_param   rejectalreadyclosedconn false ;# default: true

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

ns_section "ns/threads" {
    ns_param    stacksize           512kB
}

ns_section "ns/mimetypes" {
    ns_param    default             text/plain
    ns_param    noextension         text/plain
}

ns_section "ns/fastpath" {
    ns_param    cache               false      ;# default: false
    ns_param    cachemaxsize        10MB       ;# default: 10MB
    ns_param    cachemaxentry       8kB        ;# default: 8kB
    ns_param    mmap                false      ;# default: false
    ns_param    gzip_static         true       ;# check for static gzip; default: false
    ns_param    gzip_refresh        true       ;# refresh stale .gz files on the fly using ::ns_gzipfile
    ns_param    gzip_cmd            "/usr/bin/gzip -9"  ;# use for re-compressing
    ns_param    brotli_static       true       ;# check for static brotli files; default: false
    ns_param    brotli_refresh      true       ;# refresh stale .br files on the fly using ::ns_brotlifile
    ns_param    brotli_cmd          "/usr/bin/brotli -f -Z"  ;# use for re-compressing
    #ns_param   brotli_cmd          "/opt/local/bin/brotli -f -Z"  ;# use for re-compressing (macOS + ports)
}


########################################################################
#  Settings for the "default" server
########################################################################

ns_section "ns/server/default" {
    ns_param    enabletclpages      true  ;# default: false
    ns_param    checkmodifiedsince  false ;# default: true, check modified-since before returning files from cache. Disable for speedup
    ns_param    connsperthread      1000  ;# default: 0; number of connections (requests) handled per thread
    ns_param    minthreads          5     ;# default: 1; minimal number of connection threads
    ns_param    maxthreads          100   ;# default: 10; maximal number of connection threads
    ns_param    maxconnections      100   ;# default: 100; number of allocated connection structures
    ns_param    threadtimeout       120   ;# default: 120; timeout for idle threads
    #ns_param   concurrentcreatethreshold 100 ;# default: 80; perform concurrent creates when queue is fully beyond this percentage
					  ;# 100 is a conservative value, disabling concurrent creates
}

ns_section "ns/server/default/modules" {
    ns_param    nscp                nscp
    ns_param    nssock              nssock
    ns_param    nslog               nslog
    ns_param    nscgi               nscgi
}

ns_section "ns/server/default/fastpath" {
    ns_param    pagedir             pages
    #ns_param   serverdir           ""
    ns_param    directoryfile       "index.adp index.tcl index.html index.htm"
    ns_param    directoryproc       _ns_dirlist
    ns_param    directorylisting    fancy    ;# default: simple
    #ns_param   directoryadp       dir.adp
}

ns_section "ns/server/default/vhost" {
    ns_param    enabled             false
    ns_param    hostprefix          ""
    ns_param    hosthashlevel       0
    ns_param    stripport           true
    ns_param    stripwww            true
}

ns_section "ns/server/default/adp" {
    ns_param    map                 "/*.adp"
    ns_param    enableexpire        false    ;# default: false; set "Expires: now" on all ADP's
    #ns_param   enabledebug         true    ;# default: false
    #ns_param   enabletclpages      true     ;# default: false
    ns_param    singlescript        false    ;# default: false; collapse Tcl blocks to a single Tcl script
    ns_param    cache               false    ;# default: false; enable ADP caching
    #ns_param    cachesize           5MB
    #ns_param    bufsize             1MB
}

ns_section "ns/server/default/tcl" {
    ns_param    nsvbuckets          16       ;# default: 8
    ns_param    library             modules/tcl
    #
    # Example for initcmds (to be executed, when this server is fully initialized).
    #
    #ns_param    initcmds {
    #    ns_log notice "=== Hello World === server: [ns_info server] running"
    #}
}

ns_section "ns/server/default/module/nscgi" {
    ns_param    map                 "GET  /cgi-bin $home/cgi-bin"
    ns_param    map                 "POST /cgi-bin $home/cgi-bin"
    ns_param    interps              CGIinterps
    #ns_param   allowstaticresources true    ;# default false; serve static resources from cgi directories
}

ns_section "ns/interps/CGIinterps" {
    ns_param	.pl		    "/opt/local/bin/perl"
    ns_param	.sh		    "/bin/bash"
}

ns_section "ns/server/default/module/nslog" {
    #ns_param   file                access.log
    #ns_param   rolllog             true     ;# default: true; should server log files automatically
    #ns_param   rollonsignal        false    ;# default: false; perform roll on a sighup
    #ns_param   rollhour            0        ;# default: 0; specify at which hour to roll
    ns_param    maxbackup           7        ;# default: 10; max number of backup log files
    #ns_param   rollfmt		    %Y-%m-%d-%H:%M	;# format appended to log file name
    #ns_param   logpartialtimes     true     ;# default: false
    #ns_param   logreqtime	    true     ;# default: false; include time to service the request
    ns_param    logthreadname       true     ;# default: false; include thread name for linking with error.log

    ns_param	masklogaddr         true    ;# false, mask IP address in log file for GDPR (like anonip IP anonymizer)
    ns_param	maskipv4            255.255.255.0  ;# mask for IPv4 addresses
    ns_param	maskipv6            ff:ff:ff:ff::  ;# mask for IPv6 addresses
}

ns_section "ns/server/default/module/nssock" {
    ns_param    port                     $port
    ns_param    address                  $address
    ns_param    hostname                 [ns_info hostname]
    ns_param    maxinput                 10MB         ;# default: 1MB, maximum size for inputs (uploads)
    #ns_param   readahead                1MB          ;# default: 16384; size of readahead for requests
    ns_param    backlog                  1024         ;# default: 256; backlog for listen operations
    ns_param    acceptsize               10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
    ns_param    closewait                0            ;# default: 2; timeout in seconds for close on socket
    ns_param    maxqueuesize             1024         ;# default: 1024; maximum size of the queue
    ns_param    keepwait		 5	      ;# 5, timeout in seconds for keep-alive
    ns_param    keepalivemaxuploadsize	 0.5MB	      ;# 0, don't allow keep-alive for upload content larger than this
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
}

ns_section "ns/server/default/module/nscp" {
    ns_param   port     4080
    ns_param   address  $address
}

ns_section "ns/server/default/module/nscp/users" {
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
#ns_logctl severity Debug on
