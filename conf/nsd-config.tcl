########################################################################
# GENERATED FILE -- do not edit
# Source: conf/nsd-config.d/
########################################################################

# source: nsd-config.d/00-bootstrap.tcl
########################################################################
#
# Section 0 -- Bootstrap & defaults (pure Tcl)
#
########################################################################

if {[info commands ::ns_configure_variables] eq ""} {
    ns_log notice "backward compatibility hook (pre NaviServer 5): have to source init.tcl"
    source [file normalize [file dirname [file dirname [ns_info nsd]]]/tcl/init.tcl]
}

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
#       when nonempty, load the nscp module and listen
#       on the specified port
#   "home":
#       the root directory, containng the subdirecturies
#       "pages", "logs", "lib", "bin", "tcl", ....
#
dict set defaultConfig ipaddress   0.0.0.0
dict set defaultConfig httpport    8080
dict set defaultConfig httpsport   ""
dict set defaultConfig nscpport    ""
dict set defaultConfig home        [file dirname [file dirname [info nameofexecutable]]]
dict set defaultConfig hostname    localhost
dict set defaultConfig pagedir     {$home/pages}
dict set defaultConfig logdir      {$home/logs}
dict set defaultConfig certificate {$home/etc/server.pem}
dict set defaultConfig vhostcertificates {$home/etc/certificates}
dict set defaultConfig serverprettyname "My NaviServer Instance"
dict set defaultConfig reverseproxymode false
dict set defaultConfig trustedservers ""
dict set defaultConfig enablehttpproxy false
dict set defaultConfig setupfile ""
dict set defaultConfig max_file_upload_size  20MB
dict set defaultConfig max_file_upload_duration 5m

#
# For all potential variables defined by the dict "defaultConfig",
# allow environment variables with the prefix "nsd_" (such as
# "nsd_httpport" or "nsd_ipaddress") to override local values.
#
ns_configure_variables "nsd_" $defaultConfig

#---------------------------------------------------------------------
# Set headers that should be included in every response from the
# server.
#
set http_extraheaders {
    x-frame-options            "SAMEORIGIN"
    x-content-type-options     "nosniff"
    x-xss-protection           "1; mode=block"
    referrer-policy            "strict-origin"
}

set https_extraheaders {
    strict-transport-security "max-age=63072000; includeSubDomains"
}
append https_extraheaders $http_extraheaders

#
# Environment defaults used by OpenSSL and some tools.
# Keep these in bootstrap, since later fragments may rely on them.
#
set ::env(RANDFILE) $home/.rnd
set ::env(HOME) $home
set ::env(LANG) en_US.UTF-8

# source: nsd-config.d/10-parameters.tcl
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

# source: nsd-config.d/20-network.tcl
########################################################################
#
# Section 2 -- Global network drivers & modules
#
########################################################################

# Name of the main server configuration
set server default

#
# Collect domain names and IP addresses associated with the main server
# configuration. Network drivers use this information to validate Host
# headers and map incoming requests to the correct server.
#
set server_set [ns_set create $server]
foreach domainname $hostname {
    ns_set put $server_set $server $domainname
}
foreach address $ipaddress {
    if {[ns_ip inany $address]} continue
    ns_set put $server_set $server $address
}

if {[namespace exists ::docker] && [info commands ::docker::map_to_server] eq ""} {
    #
    # Docker support: determine externally visible host:port mappings.
    #
    # When NaviServer runs inside a container, ports are often published on the
    # Docker host with a (potentially different) external address and port.
    # The returned mappings can be used to whitelist additional Host header
    # values (e.g., "host:port") for a given server configuration.
    #
    # Legacy implementation for setups where the docker environment does not
    # provide this helper.
    #
    proc ::docker::map_external_address_to_server {server port} {
        set label "$port/tcp"
        set s [ns_set create]
        if {$port eq ""
            || ![info exists ::docker::containerMapping]
            || ![dict exists $::docker::containerMapping $label]
        } {
            return $s
        }
        foreach {k info} $::docker::containerMapping {
            if {$k ne $label} continue
            set host    [dict get $info host]
            set pubport [dict get $info port]
            if {[ns_ip valid $host] && [ns_ip inany $host]} continue
            ns_set put $s $server ${host}:${pubport}
        }
        return $s
    }
}

#
# Shared network driver defaults (for HTTP and HTTPS)
#
ns_section ns/driver/common {
    ns_param defaultserver          $server
    ns_param address                $ipaddress
    ns_param hostname               [lindex $hostname 0]
    ns_param maxinput               $max_file_upload_size
    ns_param recvwait               $max_file_upload_duration
    ns_param keepalivemaxuploadsize 100kB
    ns_param writerthreads          1
    ns_param  writersize            1kB
}

#
# Global network modules (for all servers)
#
if {[info exists httpport] && $httpport ne ""} {
    #
    # We have an "httpport" configured, so configure this module.
    #
    ns_section ns/modules {
        ns_param http nssock
    }

    ns_section -update \
        -from ns/driver/common \
        ns/module/http {
        # ns_param defaultserver  $server
        # ns_param address       $ipaddress  ;# Space separated list of IP addresses
        # ns_param hostname      [lindex $hostname 0]
        ns_param port           $httpport

        # ns_param backlog       1024         ;# default: 256; backlog for listen operations
        # ns_param acceptsize    10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
        # ns_param sockacceptlog 3            ;# ns/param sockacceptlog; report, when one accept operation
                                             ;# receives more than this threshold number of sockets
        # ns_param closewait     0s           ;# default: 2s; timeout for close on socket
        # ns_param keepwait      5s           ;# 5s, timeout for keep-alive
        # ns_param maxqueuesize  1024         ;# default: 1024; maximum size of the queue

        # ns_param readahead     1MB          ;# default: 16384; size of readahead for requests
        # ns_param maxupload     1MB          ;# default: 0, when specified, spool uploads larger than this value to a temp file

        # ns_param maxinput       $max_file_upload_size      ;# Maximum file size for uploads
        # ns_param recvwait       $max_file_upload_duration  ;# 30s, timeout for receive operations
        # ns_param keepalivemaxuploadsize   0.5MB           ;# 0, don't allow keep-alive for upload content larger than this
        # ns_param keepalivemaxdownloadsize 1MB             ;# 0, don't allow keep-alive for download content larger than this

        #
        # Spooling Threads
        #
        # ns_param  writerthreads   1           ;# default: 0, number of writer threads
        # ns_param writersize      1KB         ;# default: 1MB, use writer threads for files larger than this value
        # ns_param writerbufsize   16kB        ;# default: 8kB, buffer size for writer threads
        # ns_param writerstreaming true        ;# false;  activate writer for streaming HTML output (e.g. ns_writer)

        # ns_param spoolerthreads  1           ;# default: 0; number of upload spooler threads
        # ns_param driverthreads   2           ;# default: 1, number of driver threads (requires support of SO_REUSEPORT)

        #
        # TCP tuning
        #
        # ns_param nodelay         false       ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        # ns_param deferaccept     true        ;# false, Performance optimization

        #
        # Extra response header fields for this driver
        #
        ns_param extraheaders    $http_extraheaders
    }

    #
    # Register known Host header values for this driver and map them to server
    # configurations.
    #
    ns_section -set $server_set ns/module/http/servers {}
    if {[namespace exists ::docker]} {
        ns_section -set [::docker::map_external_address_to_server $server $httpport] ns/module/http/servers {}
    }
}

if {[info exists httpsport] && $httpsport ne ""} {
    #
    # We have an "httpsport" configured, so load and configure the
    # module "nsssl" as a global server module with the name "https".
    #
    ns_section ns/modules {
        ns_param https nsssl
    }

    ns_section -update \
        -from ns/driver/common \
        ns/module/https {
        # ns_param defaultserver $server
        # ns_param address       $ipaddress
        # ns_param hostname      [lindex $hostname 0]
        ns_param port            $httpsport

        # ns_param backlog       1024         ;# default: 256; backlog for listen operations
        # ns_param acceptsize    10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
        # ns_param sockacceptlog 3            ;# ns/param sockacceptlog; report, when one accept operation
                                              ;# receives more than this threshold number of sockets
        # ns_param closewait     0s           ;# default: 2s; timeout for close on socket
        # ns_param keepwait      5s           ;# 5s, timeout for keep-alive
        # ns_param maxqueuesize  1024         ;# default: 1024; maximum size of the queue

        # ns_param readahead     1MB          ;# default: 16384; size of readahead for requests
        # ns_param maxupload     1MB          ;# default: 0, when specified, spool uploads larger than this value to a temp file

        # ns_param maxinput      $max_file_upload_size      ;# Maximum file size for uploads
        # ns_param recvwait      $max_file_upload_duration  ;# 30s, timeout for receive operations
        # ns_param keepalivemaxuploadsize   0.5MB           ;# 0, don't allow keep-alive for upload content larger than this
        # ns_param keepalivemaxdownloadsize 1MB             ;# 0, don't allow keep-alive for download content larger than this

        #
        # Spooling Threads
        #
        # ns_param  writerthreads   1          ;# default: 0, number of writer threads
        # ns_param writersize      1KB         ;# default: 1MB, use writer threads for files larger than this value
        # ns_param writerbufsize   16kB        ;# default: 8kB, buffer size for writer threads
        # ns_param writerstreaming true        ;# false;  activate writer for streaming HTML output (e.g. ns_writer)

        # ns_param spoolerthreads  1           ;# default: 0; number of upload spooler threads
        # ns_param driverthreads   2           ;# default: 1, number of driver threads (requires support of SO_REUSEPORT)

        #
        # TCP tuning
        #
        # ns_param nodelay         false       ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        # ns_param deferaccept     true        ;# false, Performance optimization

        #
        # SSL/TLS parameters
        #
        ns_param ciphers         "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"
        #ns_param ciphersuites   "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"
        ns_param protocols       "!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"
        ns_param certificate     $certificate
        #ns_param vhostcertificates $home/etc/certificates ;# directory for vhost certificates of the default server
        ns_param verify          0

        # OCSP stapling configuration:
        # ns_param OCSPstapling    on          ;# off; activate OCSP stapling
        # ns_param OCSPstaplingVerbose  on    ;# off; make OCSP stapling more verbose
        # ns_param OCSPcheckInterval 15m      ;# default 5m; OCSP (re)check intervale

        ns_param extraheaders    $https_extraheaders
    }

    #
    # Define, which "host" (as supplied by the "host:" header field)
    # accepted over this driver should be associated with which server.
    #
    ns_section -set $server_set ns/module/https/servers {}
    if {[namespace exists ::docker]} {
        ns_section -set [::docker::map_external_address_to_server $server $httpsport] ns/module/https/servers {}
    }
}

#
# (Kept here as a minimal virtual-host sample; note that the earlier
#  ns/module/http/servers block already registers $hostname and $ipaddress.)
#
ns_section ns/module/http/servers {
    ns_param $server    localhost
    ns_param $server    [ns_info hostname]
}

# source: nsd-config.d/30-runtime.tcl
########################################################################
#
# Section 3 -- Global runtime configuration
#
########################################################################

ns_section ns/threads {
    ns_param stacksize 512kB
}

ns_section ns/mimetypes {
    ns_param default     text/plain
    ns_param noextension text/plain
}

ns_section ns/fastpath {
    #ns_param cache          false      ;# default: false
    #ns_param cachemaxsize   10MB       ;# default: 10MB
    #ns_param cachemaxentry  8kB        ;# default: 8kB
    #ns_param mmap           false      ;# default: false
    ns_param gzip_static    true        ;# check for static gzip; default: false
    ns_param gzip_refresh   true        ;# refresh stale .gz files on the fly using ::ns_gzipfile
    ns_param gzip_cmd       "/usr/bin/gzip -9"  ;# use for re-compressing
    ns_param brotli_static  true        ;# check for static brotli files; default: false
    ns_param brotli_refresh true        ;# refresh stale .br files on the fly using ::ns_brotlifile
    ns_param brotli_cmd     "/usr/bin/brotli -f -Z"  ;# use for re-compressing
    #ns_param brotli_cmd     "/opt/local/bin/brotli -f -Z"  ;# use for re-compressing (macOS + ports)
}

ns_section ns/servers {
    ns_param default $serverprettyname
}

# source: nsd-config.d/60-server-default.tcl
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

# source: nsd-config.d/70-final.tcl
########################################################################
#
# Section 7 -- Final diagnostics / sample extras
#
########################################################################

#
# For debugging, you might activate one of the following flags
#
#ns_logctl severity Debug(ns:driver) on
#ns_logctl severity Debug(request) on
#ns_logctl severity Debug(task) on
#ns_logctl severity Debug(sql) on
#ns_logctl severity Debug(nsset) on

