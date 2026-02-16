########################################################################
#
# Section 6 -- Server configurations
#
########################################################################

########################################################################
#  Settings for the "default" server
########################################################################

ns_section ns/server/default {
    ns_param enablehttpproxy     $enablehttpproxy
    ns_param enabletclpages      true  ;# default: false
    #ns_param filterrwlocks      false ;# default: true
    ns_param checkmodifiedsince  false ;# default: true, check modified-since before returning files from cache. Disable for speedup
    ns_param connsperthread      1000  ;# default: 0; number of connections (requests) handled per thread
    ns_param minthreads          5     ;# default: 1; minimal number of connection threads
    ns_param maxthreads          100   ;# default: 10; maximal number of connection threads
    #ns_param maxconnections     100   ;# default: 100; number of allocated connection structures
    ns_param rejectoverrun       true  ;# default: false; send 503 when thread pool queue overruns
    #ns_param threadtimeout      2m    ;# default: 2m; timeout for idle connection threads
    #ns_param concurrentcreatethreshold 100 ;# default: 80; perform concurrent creates when queue is fully beyond this percentage
    ;# 100 is a conservative value, disabling concurrent creates
    #ns_param connectionratelimit 200  ;# 0; limit rate per connection to this amount (KB/s); 0 means unlimited
    #ns_param poolratelimit       200  ;# 0; limit rate for pool to this amount (KB/s); 0 means unlimited

    # Extra server-specific response header fields
    #ns_param extraheaders  {referrer-policy "strict-origin"}

    # Server and version information in HTTP responses
    #ns_param noticeADP     returnnotice.adp ;# ADP file for ns_returnnotice commands (errors, redirects, ...)x
    #ns_param noticedetail  false ;# default: true; include server signature in ns_returnnotice commands (errors, redirects, ...)
    #ns_param stealthmode   true  ;# default: false; omit server header field in all responses
    #ns_param logdir        /var/logs/default
}

ns_section ns/server/default/modules {
    if {$nscpport ne ""} {ns_param nscp nscp}
    ns_param nslog      nslog
    ns_param nscgi      nscgi
    ns_param nsperm     nsperm
    ns_param revproxy   tcl
}

ns_section ns/server/default/fastpath {
    ns_param pagedir            $pagedir
    #ns_param serverdir         ""
    #ns_param directoryfile     "index.adp index.tcl index.html index.htm"
    #ns_param directoryadp      dir.adp
    #ns_param directoryproc     _ns_dirlist
    ns_param directorylisting   fancy    ;# default: simple; parameter for _ns_dirlist
    #ns_param hidedotfiles      true     ;# default: false; parameter for _ns_dirlist
}

ns_section ns/server/default/vhost {
    #ns_param enabled           false
    #ns_param hostprefix        ""
    #ns_param hosthashlevel     0
    #ns_param stripport         true
    #ns_param stripwww          true
}

ns_section ns/server/default/adp {
    ns_param map               "/*.adp"
    #ns_param enableexpire      false    ;# default: false; set "Expires: now" on all ADP's
    #ns_param enabledebug       true     ;# default: false
    #ns_param enabletclpages    true     ;# default: false
    #ns_param singlescript      false    ;# default: false; collapse Tcl blocks to a single Tcl script
    #ns_param cache             false    ;# default: false; enable ADP caching
    #ns_param cachesize         5MB
    #ns_param bufsize           1MB
}

#---------------------------------------------------------------------
# HTTP client (ns_http, ns_connchan) configuration
#---------------------------------------------------------------------
ns_section ns/server/default/httpclient {

    #
    # Set default keep-alive timeout for outgoing ns_http requests.
    # The specified value determines how long connections remain open for reuse.
    #
    #ns_param keepalive       5s       ;# default: 0s

    #
    # Default timeout to be used, when ns_http is called without an
    # explicit "-timeout" or "-expire" parameter.
    #
    #ns_param defaultTimeout  5s       ;# default: 5s

    #
    # If you wish to disable certificate validation for "ns_http" or
    # "ns_connchan" requests, set validateCertificates to false.
    # However, this is NOT recommended, as it significantly increases
    # vulnerability to man-in-the-middle attacks.
    #
    #ns_param validateCertificates false        ;# default: true

    if {[ns_config ns/server/default/httpclient validateCertificates true]} {
        #
        # Specify trusted certificates using
        #   - A single CA bundle file (CAfile) for top-level certificates, or
        #   - A directory (CApath) containing multiple trusted certificates.
        #
        # These default locations can be overridden per request in
        # "ns_http" and "ns_connchan" requests.
        #
        #ns_param CApath certificates   ;# default: [ns_info home]/certificates/
        #ns_param CAfile ca-bundle.crt  ;# default: [ns_info home]/ca-bundle.crt

        #
        # "validationDepth" sets the maximum allowed length of a certificate chain:
        #   0: Accept only self-signed certificates.
        #   1: Accept certificates issued by a single CA or self-signed.
        #   2 or higher: Accept chains up to the specified length.
        #
        #ns_param validationDepth 0   ;# default: 9

        #
        # When defining exceptions below, invalid certificates are stored
        # in the specified directory. Administrators can move these
        # certificates to the accepted certificates folder and run "openssl rehash"
        # to reduce future security warnings.
        #
        #ns_param invalidCertificates $home/invalid-certificates/   ;# default: [ns_info home]/invalid-certificates

        #
        # Define white-listed validation exceptions:
        #
        # Accept all certificates from ::1 (IPv6 loopback):
        #ns_param validationException {ip ::1}

        # For IPv4 127.0.0.1, ignore two specific validation errors:
        #ns_param validationException {ip 127.0.0.1 accept {certificate-expired self-signed-certificate}}

        # Allow expired certificates from any IP in the 192.168.1.0/24 range:
        #ns_param validationException {ip 192.168.1.0/24 accept certificate-expired}

        # Accept self-signed certificates from any IP address:
        #ns_param validationException {accept self-signed-certificate}

        # Accept all validation errors from any IP address (like disabled validation, but collects certificates)
        ns_param validationException {accept *}
    }

    #
    # Configure log file for outgoing ns_http requests
    #
    #ns_param logging        on       ;# default: off
    #ns_param logfile        httpclient.log
    #ns_param logrollfmt     %Y-%m-%d ;# format appended to log filename
    #ns_param logmaxbackup   100      ;# 10, max number of backup log files
    #ns_param logroll        true     ;# true, should server rotate log files automatically
    #ns_param logrollonsignal true    ;# false, perform log rotation on SIGHUP
    #ns_param logrollhour    0        ;# 0, specify at which hour to roll
}

ns_section ns/server/default/tcl {
    ns_param nsvbuckets    16       ;# default: 8
    ns_param nsvrwlocks    false    ;# default: true
    ns_param library       modules/tcl
    #
    # Example for initcmds (to be executed, when this server is fully initialized).
    #
    #ns_param initcmds {
    #    ns_log notice "=== Hello World === server: [ns_info server] running"
    #}
}

ns_section ns/server/default/module/nscgi {
    ns_param map          "GET  /cgi-bin $home/cgi-bin"
    ns_param map          "POST /cgi-bin $home/cgi-bin"
    ns_param interps      CGIinterps
    #ns_param allowstaticresources true    ;# default false; serve static resources from cgi directories
}

ns_section ns/interps/CGIinterps {
    ns_param .pl          "/opt/local/bin/perl"
    ns_param .sh          "/bin/bash"
}

ns_section ns/server/default/module/nslog {
    ns_param file         access.log
    #ns_param rolllog      true     ;# default: true; should server log files automatically
    #ns_param rollonsignal false    ;# default: false; perform log rotation on SIGHUP
    #ns_param rollhour     0        ;# default: 0; specify at which hour to roll
    ns_param maxbackup    7        ;# default: 10; max number of backup log files
    #ns_param rollfmt      %Y-%m-%d-%H:%M ;# format appended to log filename
    #ns_param logpartialtimes true  ;# default: false
    #ns_param logreqtime   true     ;# default: false; include time to service the request
    ns_param logthreadname true    ;# default: false; include thread name for linking with nsd.log

    ns_param masklogaddr  true     ;# false, mask IP address in log file for GDPR (like anonip IP anonymizer)
    ns_param maskipv4     255.255.255.0  ;# mask for IPv4 addresses
    ns_param maskipv6     ff:ff:ff:ff::  ;# mask for IPv6 addresses
}

ns_section ns/server/default/module/nscp {
    ns_param port         $nscpport
    ns_param address      127.0.0.1    ;# default: 127.0.0.1 or ::1 for IPv6
    #ns_param echopasswd   on           ;# default: off
    #ns_param cpcmdlogging on           ;# default: off
    #ns_param allowLoopbackEmptyUser on ;# default: off
}

ns_section ns/server/default/module/nscp/users {
    ns_param user "::"
}

ns_section ns/server/default/module/revproxy {
    ns_param verbose 1
}
