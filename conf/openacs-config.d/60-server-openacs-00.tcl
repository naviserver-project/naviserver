#=====================================================================
# Section 6.1 -- Server "openacs" ($server)
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

    # ns_param compresslevel     4         ;# default: 4; 1--9; higher = more CPU / better compression
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
    # ns_param directorylisting   fancy ;# default "simple"; can be "simple",
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
