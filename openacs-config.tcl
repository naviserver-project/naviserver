######################################################################
#
# Generic OpenACS Site Configuration for NaviServer
#
# Either provide configuration values from the command line (see
# below) or provide different values directly in the configuration
# file.
#
######################################################################

######################################################################
#
# Configuration structure overview
#
# Section 0 – Bootstrap & defaults (pure Tcl)
#    - Logging and environment checks
#    - defaultConfig dictionary and overrides (ns_configure_variables, CLI)
#    - Derivation of basic variables: server, serverroot, homedir, logdir,
#      hostname/ipaddress/httpport/httpsport, pageroot, etc.
#
# Section 1 – Global NaviServer parameters (ns/parameters)
#    - Core ns/parameters
#    - Optional reverse proxy mode (ns/parameters/reverseproxymode)
#
# Section 2 – Global network drivers & modules
#    - HTTP driver (nssock) as module "http"
#    - HTTPS driver (nsssl) as module "https"
#    - Global driver options and virtual-host mappings
#
# Section 3 – Global runtime configuration
#    - Thread library parameters (ns/threads)
#    - Extra MIME types (ns/mimetypes)
#    - Global fastpath configuration (ns/fastpath)
#
# Section 4 – Global database drivers and pools
#    - ns/db/drivers
#    - ns/db/driver-specific settings (e.g. postgres, nsoracle)
#    - ns/db/pools and pool definitions
#
# Section 5 – Global utility modules
#    - Global modules not bound to a specific server
#      (e.g. nsstats, future monitoring or helper modules)
#
# Section 6 – Server configurations
#    - Pools, redirects, ADP, Tcl, fastpath, HTTP client,
#      per-server modules;
#
# Section 7 – Final diagnostics / sample extras
#    - Final ns_log diagnostics
#
######################################################################

######################################################################
# Section 0 – Bootstrap & defaults (pure Tcl)
######################################################################
ns_log notice "nsd.tcl: starting to read configuration file..."

# Define default configuration values in a dictionary.  These can be
# overridden from ns_configure_variables, command-line options, or by
# redefining the Tcl variables later in this file.
#

set defaultConfig {
    hostname          localhost
    ipaddress         127.0.0.1
    httpport          8000
    httpsport         ""
    nscpport          ""
    smtpdport         ""
    smtprelay         $hostname:25

    server            "openacs"
    serverprettyname  "My OpenACS Instance"
    serverroot        /var/www/$server
    homedir           "[file dirname [file dirname [ns_info nsd]]]"
    logdir            $serverroot/log
    certificate       $serverroot/etc/certfile.pem
    vhostcertificates $serverroot/etc/certificates

    dbms              postgres
    db_host           localhost
    db_port           ""
    db_name           $server
    db_user           nsadmin
    db_password       ""
    db_passwordfile   ""

    CookieNamespace   ad_

    max_file_upload_size      20MB
    max_file_upload_duration   5m

    reverseproxymode  false
    trustedservers    ""
    cachingmode       full

    clusterSecret     ""
    parameterSecret   ""

    debug             false
    verboseSQL        false

    extramodules      ""
}


# Optionally override the default configuration variables defined in
# "defaultConfig" dictionary via "dict set" commands (this allows you
# to comment out lines as needed).
#
# Example: When the same domain name is used for multiple OpenACS
# instances, using the same cookie namespace for these instances can
# cause conflicts. Consider setting a unique namespace for cookies.
#
#    dict set defaultConfig CookieNamespace ad_8000_
#
#
# For Oracle, we provide different default values for convenience
#
if { [dict get $defaultConfig dbms] eq "oracle" } {
    dict set defaultConfig db_password "openacs"
    dict set defaultConfig db_name openacs
    dict set defaultConfig db_port 1521
    #
    # Traditionally, the old configs have all db_user set to the
    # db_name, e.g. "openacs".
    dict set defaultConfig db_user [dict get defaultConfig server]

    set ::env(ORACLE_HOME) /opt/oracle/product/19c/dbhome_1
    set ::env(NLS_DATE_FORMAT) YYYY-MM-DD
    set ::env(NLS_TIMESTAMP_FORMAT) "YYYY-MM-DD HH24:MI:SS.FF6"
    set ::env(NLS_TIMESTAMP_TZ_FORMAT) "YYYY-MM-DD HH24:MI:SS.FF6 TZH:TZM"
    set ::env(NLS_LANG) American_America.UTF8
}

#
# Now turn the keys from the default value dictionary into local Tcl
# variables. In this step every default value will be overwritten by a
# shell variables with the prefix "oacs_" if provided. To override
# e.g. the HTTP port, call nsd like in the following example:
#
#    oacs_httpport=8100 ... /usr/local/ns/bin/nsd ...
#

# Check for the existence of the command "ns_configure_variables".
# For backward compatibility with pre–NaviServer 5, source init.tcl if not found.
if {[info commands ::ns_configure_variables] eq ""} {
    ns_log notice "backward compatibility hook (pre NaviServer 5): have to source init.tcl"
    source [file normalize [file dirname [file dirname [ns_info nsd]]]/tcl/init.tcl]
}

ns_configure_variables "oacs_" $defaultConfig

#
# One can set here more variables (or hard-coded overwrite the values
# from the defaultConfig dictionary) here. These are standard values,
# where we assume, this are on every OpenACSS instance the same.
#
set pageroot                  ${serverroot}/www
set directoryfile             "index.tcl index.adp index.html index.htm"

#
# In case we have a db_passwordfile, use the content of the file as
# the database password. Can be used, e.g., for docker secrets.
#
if {$db_passwordfile ne "" && [file readable $db_passwordfile]} {
    try {
        set F [open $db_passwordfile]
        set db_password [string trim [read $F]]
    } finally {
        close $F
        unset F
    }
}

#
# For Oracle, we set the datasource to values which might be
# changed via environment variables. So, this has to happen
# after "ns_configure_variables"
#
if { $dbms eq "oracle" } {
    set datasource ${db_host}:${db_port}/$db_name ;# name of the pluggable database / service
} else {
    set datasource ${db_host}:${db_port}:dbname=${db_name}
}

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

#---------------------------------------------------------------------
# Set environment variables HOME and LANG. HOME is needed since
# otherwise some programs called via exec might try to write into the
# root home directory.
#
set ::env(HOME) $homedir
set ::env(LANG) en_US.UTF-8

#ns_logctl severity "Debug(ns:driver)" $debug

######################################################################
# Section 1 – Global NaviServer parameters (ns/parameters)
######################################################################
# Global NaviServer parameters
#---------------------------------------------------------------------

ns_section ns/parameters {
    #------------------------------------------------------------------
    # Core paths and process options
    #------------------------------------------------------------------

    ns_param	home		$homedir
    ns_param    logdir          $logdir
    ns_param	pidfile		nsd.pid
    ns_param	debug		$debug

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
    #   full    – normal operation (default)
    #   none    – make all ns_cache operations no-ops
    #   cluster – reserved for future clustered setups
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
    # true  (default) – reject such operations and log an error; avoids
    #                   undefined behaviour and confusing partial output.
    # false           – allow them (legacy behaviour; not recommended).
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




######################################################################
# Section 2 – Global network drivers (HTTP/HTTPS)
######################################################################
# Configuration for plain HTTP interface -- core module "nssock"
#---------------------------------------------------------------------
if {[info exists httpport] && $httpport ne ""} {
    #
    # We have an "httpport" configured, so load and configure the
    # module "nssock" as a global server module with the name "http".
    #
    ns_section ns/modules {
        ns_param http nssock
    }

    ns_section ns/module/http {

        #------------------------------------------------------------------
        # Basic binding and request size limits
        #------------------------------------------------------------------
        ns_param defaultserver $server
        ns_param address       $ipaddress
        ns_param hostname      [lindex $hostname 0]
        ns_param port          $httpport              ;# default: 80

        # Per-driver limits for incoming requests; override ns/parameters
        # maxinput/recvwait if those are set there.
        ns_param maxinput      $max_file_upload_size  ;# maximum size for request bodies (e.g. uploads)
        ns_param recvwait      $max_file_upload_duration ;# timeout for receiving the full request

        # ns_param maxline      8192   ;# default: 8192; maximum size of a single header line
        # ns_param maxheaders   128    ;# default: 128; maximum number of header lines per request
        # ns_param uploadpath   /tmp   ;# default: tmpdir; directory for temporary upload files

        # ns_param maxqueuesize 256    ;# default: 1024; maximum size of the preprocessed request queue

        #------------------------------------------------------------------
        # Socket backlog and accept behaviour
        #------------------------------------------------------------------
        # ns_param backlog       256   ;# default: listenbacklog; listen backlog for this driver;
        #                               # overrides global listenbacklog
        # ns_param acceptsize    10    ;# default: backlog; maximum number of sockets accepted
        #                               # in a single accept loop
        # ns_param sockacceptlog 3     ;# default: sockacceptlog: overrides ns/parameters sockacceptlog

        # Defer accept until data arrives (where supported). This can improve
        # performance but may cause recvwait to be ignored while the socket
        # is still in the kernel’s accept queue.
        # ns_param deferaccept   true  ;# default: false


        #------------------------------------------------------------------
        # Buffers, timeouts, and connection behaviour
        #------------------------------------------------------------------
        # ns_param bufsize       16kB  ;# default: 16kB; size of I/O buffer for reading requests
        # ns_param readahead     16kB  ;# default: bufsize; amount of extra data to read ahead
        # ns_param sendwait      30s   ;# default: 30s; timeout for sending responses
        # ns_param closewait     2s    ;# default: 2s; timeout when closing the socket
        # ns_param keepwait      2s    ;# default: 5s; keep-alive timeout

        # Control TCP_NODELAY (Nagle’s algorithm) on accepted sockets.
        #   true  – disable Nagle (set TCP_NODELAY), good for latency
        #   false – leave Nagle enabled
        # ns_param nodelay       false ;# default: true

        ns_param keepalivemaxuploadsize   100kB   ;# default: 0; # 0 = no limit; disable keep-alive
        #                                         ;# for uploads larger than this value
        # ns_param keepalivemaxdownloadsize 1MB   ;# default: 0; 0 = no limit; disable keep-alive
        #                                         ;# for responses larger than this value

        #------------------------------------------------------------------
        # Upload spooling and writer threads
        #------------------------------------------------------------------
        # Spool uploads exceeding this size to a temporary file.
        # 0 = disabled; everything stays in memory.
        ns_param maxupload          100kB  ;# default: 0; spool request bodies larger than this size

        # Use writer threads for sending large responses.
        ns_param writerthreads      2      ;# default: 0; number of writer threads (0 = disabled)
        ns_param writersize         1kB    ;# default: 1MB; use writer threads for responses larger than this size
        # ns_param writerbufsize    16kB   ;# default: 8kB; buffer size for writer threads

        # ns_param writerstreaming  true   ;# default: false; enable writer threads for streaming output (ns_write)
        # ns_param spoolerthreads   1      ;# default: 0; number of upload spooler threads

        #------------------------------------------------------------------
        # Port reuse and driver threading
        #------------------------------------------------------------------
        # Options for port reuse (see https://lwn.net/Articles/542629/).
        # These require OS support and are typically managed automatically
        # when driverthreads > 1.
        #
        # ns_param reuseport      true ;# default: false; normally not set explicitly; enabled when
        #                               # driverthreads > 1 and OS supports SO_REUSEPORT
        # ns_param driverthreads  2    ;# default: 1; number of driver threads; >1 activates reuseport

        #------------------------------------------------------------------
        # Extra response headers
        #------------------------------------------------------------------
        # Extra driver-specific response headers to be added to every
        # HTTP response sent via this driver.
        ns_param extraheaders   $http_extraheaders
    }

    # Define which Host header values should be accepted on this driver
    # and mapped to which server. The variable "hostname" may contain
    # multiple domain names, all of which are registered below.

    ns_section ns/module/http/servers {
        foreach domainname $hostname {
            ns_param $server $domainname
        }
        foreach address $ipaddress {
            if {[ns_ip inany $ipaddress]} continue
            ns_param $server $address
        }
        if {[dict exists $::docker::containerMapping $httpport/tcp]} {
            foreach {label info} $::docker::containerMapping {
                if {$label ne "$httpport/tcp"} continue
                set __host [dict get $info host]
                set __port [dict get  $info port]
                if {[ns_ip valid $__host] && [ns_ip inany $__host]} continue
                #puts "added white-listed address '${__host}:${__port}' for server $server on HTTP driver"
                ns_param $server ${__host}:${__port}
            }
        }
    }
    ns_log notice ns_configsection [ns_set format [ns_configsection ns/module/http/servers]]
}

#---------------------------------------------------------------------
# Configuration for HTTPS interface (SSL/TLS) -- core module "nsssl"
#---------------------------------------------------------------------

if {[info exists httpsport] && $httpsport ne ""} {
    #
    # We have an "httpsport" configured, so load and configure the
    # module "nsssl" as a global server module with the name "https".
    #
    ns_section ns/modules {
        ns_param https nsssl
    }

    ns_section ns/module/https {
        #------------------------------------------------------------------
        # Basic binding and request size limits
        #------------------------------------------------------------------
        ns_param defaultserver	$server
        ns_param address	$ipaddress
        ns_param port		$httpsport
        ns_param hostname	$hostname

        # Per-driver limits for incoming requests; override ns/parameters
        # maxinput/recvwait when set there.
        ns_param maxinput           $max_file_upload_size      ;# max request body size (e.g. uploads)
        ns_param recvwait           $max_file_upload_duration  ;# timeout for receiving the full request

        #------------------------------------------------------------------
        # Protocols and TLS configuration
        #------------------------------------------------------------------
        # Server certificate configuration:
        #  - "certificate" points to the main certificate/key for this driver
        #  - "vhostcertificates" is a directory with certificates for
        #    additional virtual hosts of the default server.
        ns_param certificate        $certificate
        ns_param vhostcertificates  $vhostcertificates  ;# directory for vhost certificates of the default server

        # Client certificate verification level (see nsssl docs for details)
        # ns_param verify             0  ;# default: 0

        # Cipher suites for TLS 1.2 and below.
        ns_param ciphers \
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"

        # TLS 1.3 cipher suites (if you want to override OpenSSL defaults).
        # ns_param ciphersuites "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"

        # Enabled/disabled protocol versions.
        ns_param protocols          "!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"

        # OCSP stapling configuration:
        # ns_param OCSPstapling        on   ;# default: off; enable OCSP stapling
        # ns_param OCSPstaplingVerbose on   ;# default: off; more verbose OCSP logging
        # ns_param OCSPcheckInterval   15m  ;# default: 5m; OCSP (re)check interval

        #------------------------------------------------------------------
        # Writer threads and upload handling
        #------------------------------------------------------------------
        # Spool uploads exceeding this size to a temporary file.
        # 0 = disabled; everything stays in memory.
        ns_param maxupload          100kB  ;# default: 0; spool request bodies larger than this size

        # Use writer threads for sending larger responses.
        ns_param writerthreads      2      ;# default: 0; number of writer threads (0 = disabled)
        ns_param writersize         1kB    ;# default: 1MB; use writer threads for responses larger than this size
        ns_param writerbufsize      16kB   ;# default: 8kB; buffer size for writer threads

        # ns_param writerstreaming  true   ;# default: false; enable writer threads for streaming output (ns_write)
        # ns_param spoolerthreads   1      ;# default: 0; number of upload spooler threads

        #------------------------------------------------------------------
        # Socket and connection behaviour
        #------------------------------------------------------------------
        # ns_param backlog       256   ;# default: listenbacklog; listen backlog for this driver;
        #                               # overrides global listenbacklog
        # ns_param acceptsize    10    ;# default: backlog; maximum number of sockets accepted
        #                               # in a single accept loop
        # ns_param sockacceptlog 3     ;# default: sockacceptlog: overrides ns/parameters sockacceptlog

        # Defer accept until data arrives (where supported). This can improve
        # performance but may influence how recvwait behaves while the socket
        # is still in the kernel’s accept queue.
        # ns_param deferaccept      true   ;# default: false

        # Control TCP_NODELAY (Nagle’s algorithm) on accepted sockets.
        #   true  – disable Nagle (set TCP_NODELAY), lower latency
        #   false – leave Nagle enabled
        # ns_param nodelay          false  ;# default: true

        #------------------------------------------------------------------
        # Port reuse and driver threading
        #------------------------------------------------------------------
        # Options for port reuse (see https://lwn.net/Articles/542629/).
        # These require OS support and are typically managed automatically
        # when driverthreads > 1.
        #
        # ns_param reuseport      true ;# default: false; normally not set explicitly; enabled when
        #                               # driverthreads > 1 and OS supports SO_REUSEPORT
        # ns_param driverthreads  2    ;# default: 1; number of driver threads; >1 activates reuseport

        #------------------------------------------------------------------
        # Extra response headers
        #------------------------------------------------------------------
        # Extra driver-specific response headers to be added to every HTTPS
        # response sent via this driver.
        ns_param extraheaders	$https_extraheaders
    }

    # Define which Host header values should be accepted on this HTTPS
    # driver and mapped to which server. The variable "hostname" may
    # contain multiple domain names, all of which are registered below.
    #
    ns_section ns/module/https/servers {
        foreach domainname $hostname {
            ns_param $server $domainname
        }
        foreach address $ipaddress {
            if {[ns_ip inany $address]} continue
            ns_param $server $address
        }
        if {[dict exists $::docker::containerMapping $httpsport/tcp]} {
            foreach {label info} $::docker::containerMapping {
                if {$label ne "$httpsport/tcp"} continue
                set __host [dict get $info host]
                set __port [dict get $info port]
                if {[ns_ip valid $__host] && [ns_ip inany $__host]} continue
                #puts "added white-listed address '${__host}:${__port}' for server $server on HTTP driver"
                ns_param $server ${__host}:${__port}
            }
        }
        ns_log notice ns_configsection [ns_set format [ns_configsection ns/module/https/servers]]
    }
}


######################################################################
# Section 3 – Global runtime configuration (threads, MIME types, fastpath)
######################################################################

#---------------------------------------------------------------------
# Thread library (nsthread) parameters
#---------------------------------------------------------------------
ns_section ns/threads {
    ns_param	stacksize	1MB
}

#---------------------------------------------------------------------
# Extra mime types
#---------------------------------------------------------------------
ns_section ns/mimetypes {
    #  Note: NaviServer already has an exhaustive list of MIME types:
    #  see: /usr/local/src/naviserver/nsd/mimetypes.c
    #  but in case something is missing you can add it here.

    #ns_param	default		*/*
    #ns_param	noextension	text/html
    #ns_param	.pcd		image/x-photo-cd
    #ns_param	.prc		application/x-pilot
}

#---------------------------------------------------------------------
# Global fastpath parameters
#---------------------------------------------------------------------
ns_section ns/fastpath {
    #ns_param        cache               true       ;# default: false
    #ns_param        cachemaxsize        10MB       ;# default: 10MB
    #ns_param        cachemaxentry       100kB      ;# default: 8kB
    #ns_param        mmap                true       ;# default: false
    #ns_param        gzip_static         true       ;# default: false; check for static gzip file
    #ns_param        gzip_refresh        true       ;# default: false; refresh stale .gz files
    #                                                #on the fly using ::ns_gzipfile
    #ns_param        gzip_cmd            "/usr/bin/gzip -9"  ;# use for re-compressing
    #ns_param        minify_css_cmd      "/usr/bin/yui-compressor --type css"
    #ns_param        minify_js_cmd       "/usr/bin/yui-compressor --type js"
    #ns_param        brotli_static       true       ;# default: false; check for static brotli files
    #ns_param        brotli_refresh      true       ;# default: false; refresh stale .br files
    #                                                # on the fly using ::ns_brotlifile
    #ns_param        brotli_cmd          "/usr/bin/brotli -f -Z"  ;# use for re-compressing
}

######################################################################
# Section 4 – Global database drivers and pools
######################################################################

#---------------------------------------------------------------------
# Database drivers
#
# Make sure the drivers are compiled and installed in $homedir/bin.
# Supported values for $dbms in this template:
#   oracle  – use the nsoracle driver
#   postgres – use nsdbpg (PostgreSQL)
#---------------------------------------------------------------------
ns_section ns/db/drivers {
    if { $dbms eq "oracle" } {
        set db_driver_name nsoracle
        ns_param $db_driver_name nsoracle
    } else {
        set db_driver_name postgres
        ns_param $db_driver_name nsdbpg

        # When debugging SQL, you can enable SQL-level debug messages
        # via ns_logctl. The following is an example of a useful runtime
        # setting (only available in the PostgreSQL driver).
        #
        # ns_logctl severity "Debug(sql)" -color blue $verboseSQL
    }
}

#---------------------------------------------------------------------
# Driver-specific settings
#---------------------------------------------------------------------

# Oracle driver-specific settings
ns_section ns/db/driver/nsoracle {
    # Maximum length of SQL strings to log; -1 means no limit.
    ns_param maxStringLogLength -1

    # Buffer size for LOB operations.
    ns_param LobBufferSize      32768
}

# PostgreSQL driver-specific settings
ns_section ns/db/driver/postgres {
    # Set this parameter when "psql" is not on your PATH (OpenACS specific).
    # ns_param pgbin "/usr/lib/postgresql/16/bin/"
}

#---------------------------------------------------------------------
# Database pools
#
# OpenACS uses three pools by default:
#   pool1 – main pool, most queries
#   pool2 – second pool (e.g., nested queries, not generally recommended)
#   pool3 – optional third pool, used by some packages/tools
#
# Make sure to set the db_* variables (db_host, db_port, db_name, db_user,
# db_password, db_pool, etc.) at the top of the file.
#
# In general, NaviServer can have different pools connecting to different databases
# and different database servers.  See:
#
#     http://openacs.org/doc/tutorial-second-database
#
#---------------------------------------------------------------------
ns_section ns/db/pools {
    ns_param pool1 "Pool 1"
    ns_param pool2 "Pool 2"
    ns_param pool3 "Pool 3"
}

# Pool 1 – main pool
ns_section ns/db/pool/pool1 {
    # ns_param maxidle       0     ;# time until idle connections are closed; default: 5m
    # ns_param maxopen       0     ;# max lifetime of connections; default: 60m
    # ns_param checkinterval 5m    ;# check interval for stale handles

    ns_param connections     15
    ns_param LogMinDuration  10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param logsqlerrors    $debug
    ns_param datasource      $datasource
    ns_param user            $db_user
    ns_param password        $db_password
    ns_param driver          $db_driver_name
}

# Pool 2 – secondary pool (e.g., nested queries)
ns_section ns/db/pool/pool2 {
    # ns_param maxidle       0
    # ns_param maxopen       0
    # ns_param checkinterval 5m    ;# check interval for stale handles

    # ns_param connections   2     ;# default: 2
    ns_param LogMinDuration  10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param logsqlerrors    $debug
    ns_param datasource      $datasource
    ns_param user            $db_user
    ns_param password        $db_password
    ns_param driver          $db_driver_name
}

# Pool 3 – optional third pool
ns_section ns/db/pool/pool3 {
    # ns_param maxidle       0
    # ns_param maxopen       0
    # ns_param checkinterval 5m    ;# check interval for stale handles

    # ns_param connections   2     ;# default: 2
    # ns_param LogMinDuration 0ms  ;# when SQL logging is on, log only statements above this duration
    ns_param logsqlerrors    $debug
    ns_param datasource      $datasource
    ns_param user            $db_user
    ns_param password        $db_password
    ns_param driver          $db_driver_name
}

#---------------------------------------------------------------------
# Experimental alternative DB driver -- extra module "nsdbipg"
#---------------------------------------------------------------------
if {"nsdbipg" in $extramodules} {
    ns_section ns/modules {
        ns_param nsdbipg1 nsdbipg.so
    }

    ns_section ns/module/nsdbipg1 {
        ns_param   default       true
        ns_param   maxhandles    40
        ns_param   timeout       10
        ns_param   maxidle       0
        ns_param   maxopen       0
        ns_param   maxqueries    0
        ns_param   maxrows       10000
        ns_param   datasource    "port=$db_port host=$db_host dbname=$db_name user=$db_user"
        ns_param   cachesize     [expr 1024*1024]
        ns_param   checkinterval 600
    }
}

######################################################################
# Section 5 – Global utility modules
######################################################################

#---------------------------------------------------------------------
# Statistics Module -- extra module "nsstats"
#
# When installed under acs-subsite/www/admin/nsstats.tcl it is, due to
# its /admin/ location, safe from public access.
#
# This section only configures the module; loading is optional and
# typically controlled via ns/modules (see comment below).
#---------------------------------------------------------------------
ns_section ns/module/nsstats {
    ns_param enabled  1
    ns_param user     ""
    ns_param password ""
    ns_param bglocks  {oacs:sched_procs}
}

# The nsstats module consists of a single file, there is no need to
# load it as a (Tcl) module, once the file is copied.


######################################################################
# Section 6 – Server configurations
#
#   6.1 Server "openacs" ($server)
#   6.2 ...
######################################################################

#=====================================================================
# Section 6.1 – Server "openacs" ($server)
#=====================================================================

#---------------------------------------------------------------------
# Server registration (entry in ns/servers)
#---------------------------------------------------------------------
ns_section ns/servers {
    ns_param $server $serverprettyname
}

#---------------------------------------------------------------------
# Per-server parameters and tuning
#---------------------------------------------------------------------
ns_section ns/server/$server {
    #------------------------------------------------------------------
    # Basic server settings
    #------------------------------------------------------------------
    # Default root for pages, logs, etc. If not set, NaviServer will
    # use $homedir.
    #
    # ns_param serverdir $homedir

    #------------------------------------------------------------------
    # Thread scaling, queue handling, and connection limits
    #------------------------------------------------------------------
    # Maximum number of concurrently active connections this server
    # will track. This is the number of connection structures allocated,
    # not the number of threads.
    #
    # ns_param maxconnections   100       ;# default: 100; number of allocated connection structures

    # Reject requests when the connection queue is full; the client
    # receives a 503 Service Unavailable.
    ns_param rejectoverrun   true         ;# default: false

    # Retry-After header for 503 responses (if rejectoverrun is set).
    ns_param retryafter      1s          ;# default: 5s

    # Use per-filter read/write locks instead of a single server-wide
    # lock. For most setups, the default (true) is preferable.
    # ns_param filterrwlocks    false     ;# default: true

    # Maximum and minimum number of connection threads.
    #
    # ns_param maxthreads       10        ;# default: 10; max number of connection threads
    ns_param  minthreads        2         ;# default: 1;  min number of connection threads

    # Number of requests handled by a thread before it exits.
    # 0 = unlimited (no automatic restart).
    #
    # ns_param connsperthread   1000      ;# default: 10000; 0 = unlimited
    # Setting connsperthread > 0 lets threads exit gracefully after
    # serving that many requests, which can help with Tcl-level GC.

    # Idle timeout for connection threads. Threads above "minthreads"
    # exit after being idle for this duration.
    ns_param threadtimeout      20s       ;# default: 2m

    # Watermarks for dynamic thread creation based on queue fill level.
    #
    # ns_param lowwatermark     10        ;# default: 10; create more threads above this queue fill %
    ns_param highwatermark      100       ;# default: 80; allow concurrent thread creation above this %
    #                                      # 100 disables concurrent creates

    # Optional rate limits:
    #
    # ns_param connectionratelimit 200    ;# default: 0; KB/s per connection; 0 = unlimited
    # ns_param poolratelimit       200    ;# default: 0; KB/s per pool; 0 = unlimited

    #------------------------------------------------------------------
    # HTTP response compression
    #------------------------------------------------------------------
    # Enable gzip compression for eligible responses. Individual
    # handlers can still control this via ns_conn compress.
    ns_param compressenable      on        ;# default: off

    # ns_param compresslevel     4         ;# default: 4; 1–9; higher = more CPU / better compression
    # ns_param compressminsize   512       ;# default: 512; compress responses larger than this size
    # ns_param compresspreinit   true      ;# default: false; preallocate compression buffers at startup

    #------------------------------------------------------------------
    # Directory listings
    #------------------------------------------------------------------
    # Directory listing mode. In OpenACS, directory listing handling is
    # typically done via the request processor.
    #
    # ns_param directorylisting  fancy     ;# default: "none"; "simple", "fancy" or "none", handled by OpenACS RP

    #------------------------------------------------------------------
    # HTTP response behaviour
    #------------------------------------------------------------------
    # Realm name used for HTTP Basic authentication.
    #
    # ns_param realm             yourrealm        ;# default: $server

    # Control how ns_returnnotice renders error/redirect pages.
    #
    # ns_param noticedetail      false            ;# default: true; include server signature
    # ns_param noticeadp         returnnotice.adp ;# default: returnnotice.adp

    # Miscellaneous HTTP response knobs.
    #
    # ns_param stealthmode       true             ;# default: false; omit "Server" header
    # ns_param errorminsize      0                ;# default: 514; pad error replies up to this size
    # ns_param headercase        preserve         ;# default: preserve; "preserve", "tolower", "toupper"
    # ns_param checkmodifiedsince false           ;# default: true; honour If-Modified-Since for cached files

    #------------------------------------------------------------------
    # Extra response headers (per server)
    #------------------------------------------------------------------
    # Extra server-specific response headers for all responses from
    # this server can be added here as a Tcl list of key/value pairs,
    # in the same format as "extraheaders" for the HTTP/HTTPS drivers.
    #
    # Example:
    #   {X-Frame-Options SAMEORIGIN X-Content-Type-Options nosniff}
    #
    # ns_param extraheaders {key value ...}
}


#---------------------------------------------------------------------
# Connection thread pools (optional)
#---------------------------------------------------------------------
ns_section ns/server/$server/pools {
    #
    # Define named connection pools here and configure them below.
    # These pools can be used to route specific URLs to dedicated
    # worker threads (e.g. "monitor", "fast", "slow"). All requests
    # not matching to the per-pool MAP definitions are directed to a
    # default connection pool.
    #
    # ns_param monitor "Monitoring actions to check healthiness of the system"
    # ns_param fast    "Fast requests, e.g. less than 10ms"
    # ns_param slow    "Slow lane pool, for request remapping"

    # Connection pools can be configured with the following
    # parameters, overriding the general per-server settings.
    #
    #       map
    #       connsperthread
    #       highwatermark
    #       lowwatermark
    #       maxconnections
    #       rejectoverrun
    #       retryafter
    #       maxthreads
    #       minthreads
    #       poolratelimit
    #       connectionratelimit
    #       threadtimeout
}

ns_section ns/server/$server/pool/monitor {
    ns_param minthreads 2
    ns_param maxthreads 2

    # Map specific HTTP methods + URLs to this pool:
    ns_param   map "GET /SYSTEM"
    ns_param   map "GET /acs-admin"
    ns_param   map "GET /admin/nsstats"
    ns_param   map "GET /ds"
    ns_param   map "GET /request-monitor"
    ns_param   map "POST /SYSTEM"
    ns_param   map "POST /acs-admin"
    ns_param   map "POST /admin/nsstats"
    ns_param   map "POST /ds"
}


ns_section ns/server/$server/pool/fast {
    ns_param minthreads      2
    ns_param maxthreads      2
    ns_param   rejectoverrun true

    ns_param   map "GET /*.png"
    ns_param   map "GET /*.PNG"
    ns_param   map "GET /*.jpg"
    ns_param   map "GET /*.pdf"
    ns_param   map "GET /*.gif"
    ns_param   map "GET /*.mp4"
    ns_param   map "GET /*.ts"
    ns_param   map "GET /*.m3u8"
}

ns_section ns/server/$server/pool/slow {
    ns_param minthreads     2
    ns_param maxthreads     5
    ns_param maxconnections 600
    ns_param rejectoverrun  true

    #
    # The mapping for this pool is managed programmatically via the
    # xotcl-request monitor.
    #
}

#---------------------------------------------------------------------
# Special HTTP pages (error redirects)
#---------------------------------------------------------------------
ns_section ns/server/$server/redirects {
    ns_param 403 /shared/403
    ns_param 404 /shared/404
    ns_param 500 /shared/500
    ns_param 503 /shared/503
}


#---------------------------------------------------------------------
# ADP (template) configuration
#---------------------------------------------------------------------
ns_section ns/server/$server/adp {
    ns_param	enabledebug	$debug
    ns_param	map		/*.adp		;# Extensions to parse as ADP's
    # ns_param	map		"/*.html"	;# Any extension can be mapped
    #
    # ns_param	cache		true		;# default: false; enable ADP caching
    # ns_param	cachesize	10MB		;# default: 5MB; size of ADP cache
    # ns_param	bufsize		5MB		;# default: 1MB; size of ADP buffer
    #
    # ns_param	trace		true		;# default: false; trace execution of adp scripts
    # ns_param	tracesize	100		;# default: 40; max number of entries in trace
    #
    # ns_param	stream		true		;# default: false; enable ADP streaming
    # ns_param	enableexpire	true		;# default: false; set "Expires: now" on all ADP's
    # ns_param	safeeval	true		;# default: false; disable inline scripts
    # ns_param	singlescript	true		;# default: false; collapse Tcl blocks to a single Tcl script
    # ns_param	detailerror	false		;# default: true;  include connection info in error backtrace
    # ns_param	stricterror	true		;# default: false; interrupt execution on any error
    # ns_param	displayerror	true		;# default: false; include error message in output
    # ns_param	trimspace	true		;# default: false; trim whitespace from output buffer
    # ns_param	autoabort	false		;# default: true;  failure to flush a buffer (e.g. closed HTTP connection) generates an ADP exception
    #
    # ns_param	errorpage	/.../errorpage.adp	;# page for returning errors
    # ns_param	startpage	/.../startpage.adp	;# file to be run for every adp request; should include "ns_adp_include [ns_adp_argv 0]"
    # ns_param	debuginit	some-proc		;# ns_adp_debuginit; proc to be executed on debug init
    #
}

ns_section ns/server/$server/adp/parsers {
    ns_param	fancy		".adp"
}

#---------------------------------------------------------------------
# Tcl configuration
#---------------------------------------------------------------------
ns_section ns/server/$server/tcl {
    ns_param debug   $debug
    ns_param library $serverroot/tcl                     ;# default: modules/tcl
    # ns_param initfile ${serverroot}/tcl/server-init.tcl ;# default: bin/init.tcl

    #------------------------------------------------------------------
    # NSV (shared arrays) configuration
    #------------------------------------------------------------------
    # ns_param	nsvbuckets	16       ;# default: 8
    # ns_param	nsvrwlocks      false    ;# default: true

    #------------------------------------------------------------------
    # Server initialization (executed, when server is up)
    #------------------------------------------------------------------
    # ns_param initcmds {
    #    ns_log notice "=== Hello World === server: [ns_info server]"
    # }
}


#---------------------------------------------------------------------
# Per-server fastpath (static file) configuration
#---------------------------------------------------------------------
ns_section ns/server/$server/fastpath {
    ns_param pagedir $pageroot

    # ns_param directoryfile      "index.adp index.tcl index.html index.htm"
    # ns_param directoryadp       $pageroot/dirlist.adp ;# default: ""
    # ns_param directoryproc      _ns_dirlist           ;# default: "_ns_dirlist"
    # ns_param	directorylisting  fancy ;# default "simple"; can be "simple",
    #                                   ;# "fancy" or "none"; parameter for _ns_dirlist
    # ns_param hidedotfiles       true  ;# default: false
}


#---------------------------------------------------------------------
# HTTP client (ns_http, ns_connchan) configuration
#---------------------------------------------------------------------
ns_section ns/server/$server/httpclient {
    #
    # Set default keep-alive timeout for outgoing ns_http requests.
    #ns_param	keepalive       5s       ;# default: 0s

    #
    # Default timeout to be used, when ns_http is called without an
    # explicit "-timeout" or "-expire" parameter.
    #
    #ns_param	defaultTimeout  5s       ;# default: 5s

    #------------------------------------------------------------------
    # TLS / certificate validation for outgoing requests:
    #------------------------------------------------------------------
    # If you wish to disable certificate validation for "ns_http" or
    # "ns_connchan" requests, set validateCertificates to false.
    # However, this is NOT recommended, as it significantly increases
    # vulnerability to man-in-the-middle attacks.
    #
    #ns_param validateCertificates false        ;# default: true

    if {[ns_config ns/server/$server/httpclient validateCertificates true]} {
        #
        # Specify trusted certificates using
        #   - A single CA bundle file (cafile) for top-level certificates, or
        #   - A directory (capath) containing multiple trusted certificates.
        #
        # These default locations can be overridden per request in
        # "ns_http" and "ns_connchan" requests.
        #
        #ns_param capath certificates   ;# default: [ns_info home]/certificates/
        #ns_param cafile ca-bundle.crt  ;# default: [ns_info home]/ca-bundle.crt

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
        #ns_param invalidCertificates $homedir/invalid-certificates/   ;# default: [ns_info home]/invalid-certificates

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

    #------------------------------------------------------------------
    # Logging for outgoing HTTP (ns_http only):
    #------------------------------------------------------------------
    #ns_param	logging		on       ;# default: off
    #ns_param	logfile		httpclient.log
    #ns_param	logrollfmt	%Y-%m-%d ;# format appended to log filename
    #ns_param	logmaxbackup	100      ;# default: 10; max number of backup log files
    #ns_param	logroll		true     ;# default: true; should server rotate log files automatically
    #ns_param	logrollonsignal	true     ;# default: false; perform log rotation on SIGHUP
    #ns_param	logrollhour	0        ;# default: 0; specify at which hour to roll
}

#=====================================================================
# Per-server modules
#=====================================================================

#---------------------------------------------------------------------
# Server's DB configuration -- core module "nsdb"
#---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    ns_param nsdb    nsdb
}
ns_section ns/server/$server/db {
    ns_param pools       pool1,pool2,pool3
    ns_param defaultpool pool1
}

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
    # ns_param rollhour    0       ;# default: 0; hour of day to roll (0–23)
    # ns_param rollonsignal true   ;# default: false; rotate log on SIGHUP

    # Suffix appended to the log filename when rolling.
    ns_param rollfmt      %Y-%m-%d
}

#---------------------------------------------------------------------
# NaviServer Control Port -- core module "nscp"
# ---------------------------------------------------------------------
# This module lets you connect to a specified host and port using a
# telnet client to administer the server and execute database commands
# on the running system.
#
# Details about enabling and configuration:
#     https://naviserver.sourceforge.io/n/nscp/files/nscp.html
#
ns_section "ns/server/$server/modules" {
    if {$nscpport ne ""} {ns_param nscp nscp}
}
ns_section "ns/server/$server/module/nscp" {
    ns_param port $nscpport
    ns_param address  127.0.0.1        ;# default: 127.0.0.1 or ::1 for IPv6
    #ns_param echopasswd on            ;# default: off
    ns_param cpcmdlogging on           ;# default: off
    #ns_param allowLoopbackEmptyUser on ;# default: off
}
ns_section "ns/server/$server/module/nscp/users" {
    ns_param user "nsadmin:t2GqvvaiIUbF2:"
}

#---------------------------------------------------------------------
# NaviServer NaviServer Process Proxy -- core module "nsproxy"
# ---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    ns_param nsproxy nsproxy
}
ns_section ns/server/$server/module/nsproxy {
    # ns_param	maxworker         8     ;# default: 8
    # ns_param	sendtimeout       5s    ;# default: 5s
    # ns_param	recvtimeout       5s    ;# default: 5s
    # ns_param	waittimeout       100ms ;# default: 1s
    # ns_param	idletimeout       5m    ;# default: 5m
    # ns_param	logminduration    1s    ;# default: 1s
}

#---------------------------------------------------------------------
# CGI interface -- core module "nscgi"
#---------------------------------------------------------------------
# ns_section ns/server/$server/modules {
#     ns_param	nscgi nscgi
# }
# ns_section ns/server/$server/module/nscgi {
#     ns_param  map	"GET  /cgi-bin ${serverroot}/cgi-bin"
#     ns_param  map	"POST /cgi-bin ${serverroot}/cgi-bin"
#     ns_param  Interps CGIinterps
#     ns_param  allowstaticresources true    ;# default: false
# }
# ns_section ns/interps/CGIinterps {
#     ns_param .pl "/usr/bin/perl"
# }


#---------------------------------------------------------------------
# Tcl Thread library -- extra module "libthread"
# ---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    #
    # Determine, if libthread is installed. First check for a version
    # having the "-ns" suffix. If this does not exist, check for a
    # legacy version without it.
    #
    set libthread \
        [lindex [lsort [glob -nocomplain \
                            $homedir/lib/thread*/libthread-ns*[info sharedlibextension]]] end]
    if {$libthread eq ""} {
        set libthread \
            [lindex [lsort [glob -nocomplain \
                                $homedir/lib/thread*/lib*thread*[info sharedlibextension]]] end]
    }
    if {$libthread eq ""} {
        ns_log notice "No Tcl thread library installed in $homedir/lib/"
    } else {
        ns_param	libthread $libthread
        ns_log notice "Use Tcl thread library $libthread"
    }
}

#---------------------------------------------------------------------
# SMTPD proxy/server for NaviServer -- extra module "nssmtpd"
# ---------------------------------------------------------------------
# Outgoing mail for OpenACS
#
# To use this module:
#   1. Install the NaviServer nssmtpd module.
#   2. Set a nonempty $smtpdport.
#   3. Set the OpenACS package parameter
#        EmailDeliveryMode = nssmtpd
#      in acs-mail-lite. See:
#      https://openacs.org/xowiki/outgoing_email
#
ns_section ns/server/$server/modules {
    if {$smtpdport ne ""} {ns_param nssmtpd nssmtpd}
}
ns_section "ns/server/$server/module/nssmtpd" {
    #------------------------------------------------------------------
    # Basic binding and SMTP behaviour
    #------------------------------------------------------------------
    ns_param port        $smtpdport
    ns_param address     127.0.0.1            ;# local interface for SMTP server
    ns_param relay       $smtprelay           ;# upstream MTA or mail relay (e.g. localhost:25)
    ns_param spamd       localhost            ;# spamd/spamassassin daemon for filtering

    # SMTP processing callbacks (implemented in Tcl)
    ns_param initproc    smtpd::init
    ns_param rcptproc    smtpd::rcpt
    ns_param dataproc    smtpd::data
    ns_param errorproc   smtpd::error

    # Domain handling
    ns_param relaydomains "localhost"
    ns_param localdomains "localhost"

    #------------------------------------------------------------------
    # Logging and log rotation
    #------------------------------------------------------------------
    ns_param logging     on                   ;# default: off
    ns_param logfile     smtpsend.log
    ns_param logrollfmt  %Y-%m-%d             ;# appended to log filename on rotation

    # Optional rotation controls:
    #
    # ns_param logmaxbackup   100             ;# default: 10; max number of rotated logs
    # ns_param logroll        true            ;# default: true; auto-rotate logs
    # ns_param logrollonsignal true           ;# default: false; rotate on SIGHUP
    # ns_param logrollhour    0               ;# default: 0; hour of day for rotation

    #------------------------------------------------------------------
    # STARTTLS configuration (optional)
    #------------------------------------------------------------------
    # Enable STARTTLS and specify certificate chain files if needed.
    #
    # ns_param certificate "path/to/your/certificate-chain.pem"
    # ns_param cafile      ""
    # ns_param capath      ""
    #
    # Cipher suite selection (TLS 1.2 and below):
    # ns_param ciphers     "..."
}



#---------------------------------------------------------------------
# WebSocket -- extra module "websocket"
#---------------------------------------------------------------------
# ns_section "ns/server/$server/modules" {
#    ns_param websocket tcl
# }
# ns_section "ns/server/$server/module/websocket/chat" {
#    ns_param urls     /websocket/chat
# }
# ns_section "ns/server/$server/module/websocket/log-view" {
#    ns_param urls     /admin/websocket/log-view
#    ns_param refresh  1000   ;# refresh time for file watcher in milliseconds
# }

#---------------------------------------------------------------------
# Interactive Shell for NaviServer -- extra module "nsshell"
#---------------------------------------------------------------------
# ns_section "ns/server/$server/modules" {
#     ns_param    nsshell   tcl
# }
#
# ns_section "ns/server/$server/module/nsshell" {
#     ns_param    url                 /nsshell
#     ns_param    kernel_heartbeat    5
#     ns_param    kernel_timeout      10
# }

#---------------------------------------------------------------------
# Web Push for NaviServer -- extra module "nswebpush"
#---------------------------------------------------------------------
# ns_section ns/server/$server/modules {
#    ns_param nswebpush tcl
# }

#---------------------------------------------------------------------
# Let's Encrypt -- extra module "letsencrypt"
#---------------------------------------------------------------------
ns_section "ns/server/$server/modules" {
    #ns_param letsencrypt tcl
}
ns_section ns/server/$server/module/letsencrypt {

    # Provide one or more domain names (latter for multi-domain SAN
    # certificates). These values are a default in case the domains
    # are not provided by other means (e.g. "letsencrypt.tcl").  In
    # case multiple NaviServer virtual hosts are in used, this
    # definition must be on the $server, which is used for
    # obtaining updates (e.g. main site) although it retrieves a
    # certificate for many subsites.

    #ns_param domains { openacs.org openacs.net fisheye.openacs.org cvs.openacs.org }
}

#---------------------------------------------------------------------
# PAM authentication -- extra module "nspam"
#---------------------------------------------------------------------
# ns_section ns/server/$server/modules {
#     ns_param	nspam nspam
# }
# ns_section ns/server/$server/module/nspam {
#     ns_param	PamDomain "pam_domain"
# }



#---------------------------------------------------------------------
# OpenACS-specific server general configuration
#---------------------------------------------------------------------
# Define/override OpenACS kernel parameter for $server
#
ns_section ns/server/$server/acs {
    #------------------------------------------------------------------
    # Cookie namespace and static CSP rules
    #------------------------------------------------------------------
    # Optionally use a different cookie namespace (used as a prefix for
    # OpenACS cookies). This is important when, for example, multiple
    # servers run on different ports of the same host but must not share
    # login/session cookies.
    #
    ns_param CookieNamespace $CookieNamespace

    # Mapping between MIME types and CSP rules for static files.
    # The value is a Tcl dict, used e.g. by
    #   security::csp::add_static_resource_header
    #
    # Example below disables script execution from inline SVG images.
    #
    ns_param StaticCSP {
        image/svg+xml "script-src 'none'"
    }

    #------------------------------------------------------------------
    # Host header validation (OpenACS level)
    #------------------------------------------------------------------
    # Whitelist for Host header values, as seen by OpenACS.
    #
    # The configuration file may contain a list of hostnames accepted
    # as values of the Host header field (typically domain name with
    # optional port). Validation is needed, for example, to avoid
    # accepting spoofed host headers that could hijack redirects to a
    # different site. This is usually necessary when running behind a
    # proxy or in containerized setups where the Host header does not
    # directly match any driver configuration.
    #
    ns_param whitelistedHosts {}

    #------------------------------------------------------------------
    # Deprecated code loading (compatibility)
    #------------------------------------------------------------------
    # The parameter "WithDeprecatedCode" controls whether OpenACS
    # core/library files should load deprecated compatibility shims.
    # Set this to 1 for legacy sites that still rely on old APIs.
    #
    # Note: Setting this parameter to 0 may break packages that still
    # depend on deprecated interfaces.
    #
    # ns_param WithDeprecatedCode true    ;# default: false

    #------------------------------------------------------------------
    # Server restart behaviour (platform-specific)
    #------------------------------------------------------------------
    # When set to 1, acs-admin/server-restart uses "ns_shutdown -restart"
    # instead of plain "ns_shutdown". This is required on some Windows
    # installations. Default is 0.
    #
    # ns_param NsShutdownWithNonZeroExitCode 1

    #------------------------------------------------------------------
    # Logging and privacy
    #------------------------------------------------------------------
    # Include user_ids in log files? Some sensitive sites forbid this.
    # Default is 0 (do not include user_ids in logs).
    #
    # ns_param LogIncludeUserId 1

    #------------------------------------------------------------------
    # Cluster and security secrets
    #------------------------------------------------------------------
    # Cluster secret for intra-cluster communication. Clustering will
    # not be enabled if no value is provided.
    #
    ns_param clusterSecret   $clusterSecret

    # Secret used for signing query and form parameters (e.g. security
    # tokens in URLs and forms).
    #
    ns_param parameterSecret             $parameterSecret
}

#---------------------------------------------------------------------
# OpenACS-specific server per package configuration
#---------------------------------------------------------------------
# Define/override OpenACS package parameters in section
# ending with /acs/PACKAGENAME
#
ns_section ns/server/$server/acs/acs-tcl {
    # Example cache sizes; adjust for your installation:
    #
    # ns_param SiteNodesCacheSize        2000000
    # ns_param SiteNodesIdCacheSize       100000
    # ns_param SiteNodesChildenCacheSize  100000
    # ns_param SiteNodesPrefetch  {/file /changelogs /munin}
    # ns_param UserInfoCacheSize          2000000
}


ns_section ns/server/$server/acs/acs-mail-lite {
    # Setting EmailDeliveryMode to "log" is useful for developer
    # instances. Typically set in OpenACS package parameters, be we
    # can override here.
    # ns_param EmailDeliveryMode log      ;# or "nssmtpd" when using the nssmtpd module
}

ns_section ns/server/$server/acs/acs-api-browser {
    # ns_param IncludeCallingInfo true    ;# useful mostly on development instances
}

#---------------------------------------------------------------------
# WebDAV support (optional; requires oacs-dav)
#---------------------------------------------------------------------
#ns_section ns/server/$server/tdav {
#    ns_param	propdir            $serverroot/data/dav/properties
#    ns_param	lockdir            $serverroot/data/dav/locks
#    ns_param	defaultlocktimeout 300
#}
#
#ns_section ns/server/$server/tdav/shares {
#    ns_param	share1		"OpenACS"
#}
#
#ns_section ns/server/$server/tdav/share/share1 {
#    ns_param	uri		"/dav/*"
#    ns_param	options		"OPTIONS COPY GET PUT MOVE DELETE HEAD MKCOL POST PROPFIND PROPPATCH LOCK UNLOCK"
#}


#=====================================================================
# Section 6.2 – additional servers
#=====================================================================

#
# Add more server configurations here when needed.
#


######################################################################
# Section 7 – Final diagnostics / sample extras
######################################################################

#ns_logctl severity Debug(ns:driver) on
#ns_logctl severity Debug(request) on
#ns_logctl severity Debug(task) on
#ns_logctl severity Debug(connchan) on
ns_logctl severity debug $debug
ns_logctl severity "Debug(sql)" $verboseSQL

#
# In case, you want to activate (more intense) SQL logging at runtime,
# consider the two commands (e.g. entered over ds/shell)
#
#    ns_logctl severity "Debug(sql)" on
#    ns_db logminduration pool1  10ms
#

# If you want to activate core dumps, one can use the following command
#
#ns_log notice "nsd.tcl: ns_rlimit coresize [ns_rlimit coresize unlimited]"

ns_log notice "nsd.tcl: using threadsafe tcl: [info exists ::tcl_platform(threaded)]"
ns_log notice "nsd.tcl: finished reading configuration file."
