######################################################################
#
# Config parameter for an OpenACS site using NaviServer.
#
# These default settings will only work in limited circumstances.
# Two servers with default settings cannot run on the same host
#
######################################################################
ns_log notice "nsd.tcl: starting to read configuration file..."

#---------------------------------------------------------------------
# Port settings:
#
#    Change the HTTP and HTTPS port to e.g. 80 and 443 for production
#    use.  Setting the configuration parameter "httpport" or
#    "httpsport" to the special value 0 means to active the HTTP/HTTPS
#    driver for ns_http, but do not listen on this port. Without
#    loading the driver, ns_http won't be able to the protocol.
#
# Note: If the specufued port is privileged (usually < 1024), OpenACS
# must be started by root, and the run script must contain the flag
# '-b address:port' which matches the configured address and port.
#
# The "hostname" (e.h. domain names) and "ipaddress" should be set to
# actual values such that the server is reachable over the
# Internet. The default values are fine for testing purposes. One can
# specify for "hostname" and "ipaddress" also multiple values
# (e.g. IPv4 and IPv6). Multiple hostnames are used as alternative
# domain names names for the "http" and "https" server sections.
#
#    hostname	localhost
#    ipaddress	127.0.0.1  ;# listen on loopback via IPv4
#    ipaddress	0.0.0.0    ;# listen on all IPv4 addresses
#    ipaddress  ::1        ;# listen on loopback via IPv6
#    ipaddress	::0        ;# listen on all IPv6 addresses
#
# All default variables in "defaultConfig" can be overloaded by:
#
# 1) Setting these variables explicitly in this file after
#    "ns_configure_variables" (highest precedence)
#
# 2) Setting these variables as environment variables with the "oacs_"
#    prefix (suitable for e.g. docker setups).  The lookup for
#    environment variables happens in "ns_configure_variables".
#
# 3) Alter/override the variables in the "defaultConfig"
#    (lowest precedence)
#
set defaultConfig {
    hostname	localhost
    ipaddress	127.0.0.1
    httpport	8000
    httpsport	""
    nscpport    ""
    smtpdport   ""

    server           "openacs"
    serverroot        /var/www/$server
    logdir            $serverroot/log
    homedir           /usr/local/ns
    bindir            $homedir/bin
    certificate       $serverroot/etc/certfile.pem
    vhostcertificates $serverroot/etc/certificates

    db_name           $server
    db_user           nsadmin
    db_password       ""
    db_host           localhost
    db_port           ""
    db_passwordfile   ""

    CookieNamespace   ad_
    cachingmode       full
    serverprettyname  "My OpenACS Instance"

    reverseproxymode  false
    trustedservers    ""

    clusterSecret     ""
    parameterSecret   ""
}

#
# Override default variables as defined by "defaultConfig" (this
# allows commenting lines)
#
# If the same domain name serves multiple OpenACS instances,
# same-named cookies will mix up.  You might consider a different
# namespace for the cookies.
#
#dict set defaultConfig CookieNamespace ad_8000_

#---------------------------------------------------------------------
# Which DBMS do you want to use? PostgreSQL or Oracle?
#
set dbms postgres

#
# For Oracle, some of the defaults have to be adjusted,
# make also sure that certain environment variables are set
#
if { $dbms eq "oracle" } {
    dict set defaultConfig db_password "openacs"
    dict set defaultConfig db_name openacs
    dict set defaultConfig db_port 1521
    #
    # Traditionally, the old configs have all db_user set to the
    # db_name, e.g. "openacs".
    dict set defaultConfig db_user $server

    set ::env(ORACLE_HOME) /opt/oracle/product/19c/dbhome_1
    set ::env(NLS_DATE_FORMAT) YYYY-MM-DD
    set ::env(NLS_TIMESTAMP_FORMAT) "YYYY-MM-DD HH24:MI:SS.FF6"
    set ::env(NLS_TIMESTAMP_TZ_FORMAT) "YYYY-MM-DD HH24:MI:SS.FF6 TZH:TZM"
    set ::env(NLS_LANG) American_America.UTF8
}

#---------------------------------------------------------------------
# If debug is false, all debugging will be turned off.
set debug false
set verboseSQL false

set max_file_upload_size      20MB
set max_file_upload_duration   5m

#---------------------------------------------------------------------
# Set headers that should be included in every response from the
# server.
#
set http_extraheaders {
    X-Frame-Options            "SAMEORIGIN"
    X-Content-Type-Options     "nosniff"
    X-XSS-Protection           "1; mode=block"
    Referrer-Policy            "strict-origin"
}

set https_extraheaders {
    Strict-Transport-Security "max-age=31536000; includeSubDomains"
}
append https_extraheaders $http_extraheaders

######################################################################
#
# End of instance-specific settings
#
# Nothing below this point need be changed in a default install.
#
######################################################################
#
# For all potential variables defined by the dict "defaultConfig",
# allow environment variables such as "oacs_httpport" or
# "oacs_ipaddress" to override local values.
#
source [file dirname [ns_info nsd]]/../tcl/init.tcl
ns_configure_variables "oacs_" $defaultConfig

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
# Set environment variables HOME and LANG. HOME is needed since
# otherwise some programs called via exec might try to write into the
# root home directory.
#
set ::env(HOME) $homedir
set ::env(LANG) en_US.UTF-8


ns_logctl severity "Debug(ns:driver)" $debug

#---------------------------------------------------------------------
#
# NaviServer's directories. Auto-configurable.
#
#---------------------------------------------------------------------
# Where are your pages going to live ?
set pageroot                  ${serverroot}/www
set directoryfile             "index.tcl index.adp index.html index.htm"

#---------------------------------------------------------------------
# Global NaviServer parameters
#---------------------------------------------------------------------
ns_section ns/parameters {
    ns_param	serverlog	${logdir}/error.log
    ns_param	pidfile		${logdir}/nsd.pid
    ns_param	home		$homedir
    ns_param	debug		$debug

    # Define optionally the tmpdir. If not specified, the
    # environment variable TMPDIR is used. If that is not
    # specified either, a system specific constant us used
    # (compile time macro P_tmpdir)
    #
    # ns_param        tmpdir    c:/tmp

    # Parameter for controlling caching via ns_cache. Potential values
    # are "full" or "none", future versions might allow as well
    # "cluster".  The value of "none" makes ns_cache operations to
    # no-ops, this is a very conservative value for clusters.
    #
    ns_param   cachingmode     $cachingmode  ;# default: "full"

    # Timeout for shutdown to let existing connections and background
    # jobs finish.  When this time limit is exceeded the server shuts
    # down immediately.
    #
    # ns_param shutdowntimeout 20s  ;# 20s is the default

    # Configuration of incoming connections
    #
    # ns_param listenbacklog   256  ;# default: 32; global backlog for ns_socket commands
                                     # and global default for drivers; can be refined
                                     # by driver parameter "backlog".
    # ns_param sockacceptlog     3  ;# default: 4; report, when one accept operation receives
                                     # more than this threshold number of sockets; can be refined
                                     # by driver parameter with the same name.

    #
    # Configuration of outgoing requests via ns_http
    #
    # ns_param    nshttptaskthreads  2     ;# default: 1; number of task threads for ns_http when running detached
    # ns_param    autosni            false ;# default: true

    #
    # Configuration of error.log
    #
    # Rolling of logfile:
    ns_param	logroll		on
    ns_param	logmaxbackup	100      ;# 10 is default
    ns_param	logrollfmt	%Y-%m-%d ;# format appended to serverlog filename when rolled
    #
    # Format of log entries in serverlog:
    # ns_param  logsec          false    ;# add timestamps in second resolution (default: true)
    # ns_param  logusec         true     ;# add timestamps in microsecond (usec) resolution (default: false)
    # ns_param  logusecdiff     true     ;# add timestamp diffs since in microsecond (usec) resolution (default: false)
    # ns_param  logthread       false    ;# add thread-info the log file lines (default: true)
    ns_param	logcolorize	true     ;# colorize log file with ANSI colors (default: false)
    ns_param	logprefixcolor	green    ;# black, red, green, yellow, blue, magenta, cyan, gray, default
    # ns_param  logprefixintensity normal;# bright or normal
    #
    # Severities to be logged (can be better controlled (also at runtime) via ns_logctl)
    #ns_param	logdebug	trueug   ;# debug messages
    #ns_param	logdev		true     ;# development message
    #ns_param   lognotice       true     ;# informational messages
    #ns_param   sanitizelogfiles 2       ;# default: 2; 0: none, 1: full, 2: human-friendly

    # ns_param	mailhost            localhost

    # ns_param	jobsperthread       0     ;# number of ns_jobs before thread exits
    # ns_param	jobtimeout          5m    ;# default "ns_job wait" timeout
    # ns_param	joblogminduration   1s    ;# default: 1s

    # ns_param	schedsperthread     0     ;# number of scheduled jobs before thread exits
    # ns_param	schedlogminduration 2s    ;# print warnings when scheduled job takes longer than that

    # Write asynchronously to log files (access log and error log)
    # ns_param	asynclogwriter	true		;# false

    #ns_param       mutexlocktrace       true   ;# default false; print durations of long mutex calls to stderr

    # Reject output operations on already closed or detached connections (e.g. subsequent ns_return statements)
    #ns_param       rejectalreadyclosedconn false ;# default: true

    # Allow concurrent create operations of Tcl interpreters.
    # Versions up to at least Tcl 8.5 are known that these might
    # crash in case two threads create interpreters at the same
    # time. These crashes were hard to reproduce, but serializing
    # interpreter creation helped. Starting with Tcl 8.6,
    # the default is set to "true".
    #ns_param        concurrentinterpcreate false   ;# default: true

    # Enforce sequential thread initialization. This is not really
    # desirably in general, but might be useful for hunting strange
    # crashes or for debugging with valgrind.
    # ns_param	tclinitlock	true           ;# default: false

    #
    # Encoding settings
    #

    # NaviServer's defaults charsets are all utf-8.  Although the
    # default charset is utf-8, set the parameter "OutputCharset"
    # here, since otherwise OpenACS uses in the meta-tags the charset
    # from [ad_conn charset], which is taken from the db and is
    # per-default ISO-8859-1.
    ns_param	OutputCharset	utf-8
    # ns_param	URLCharset	utf-8

    # In cases were UTF-8 parsing fails in forms, retry with the specified charset.
    # ns_param formfallbackcharset iso8859-1
    #
    # DNS configuration parameters
    #
    #ns_param dnscache true          ;# default: true
    #ns_param dnswaittimeout 5s      ;# time for waiting for a DNS reply; default: 5s
    #ns_param dnscachetimeout 60m     ;# time to keep entries in cache; default: 1h
    ns_param dnscachemaxsize 500KB  ;# max size of DNS cache in memory units; default: 500KB

    # Running behind proxy? Used also by OpenACS.  This parameter is
    # outdated and kept here for backward compatibility. Use the
    # section ns/parameters/reverseproxymode instead.
    ns_param reverseproxymode	$reverseproxymode
}

#
# When running behind a reverse proxy, use the following parameters
#
ns_section ns/parameters/reverseproxymode {
    ns_param enabled        $reverseproxymode
    #
    # When defining "trustedservers", the X-Forwarded-For header field
    # is only accepted in requests received from one of the specified
    # servers. The list of servers can be provided by using IP
    # addresses or CIDR masks. Additionally, the processing mode of
    # the contents of the X-Forwarded-For contents switches to
    # right-to-left, skipping trusted servers. So, the dangerof
    # obtaining spoofed addresses can be reduced.
    #
    ns_param trustedservers $trustedservers
    #
    # Optionally, non-public entries in the content of X-Forwarded-For
    # can be ignored. These are not useful for e.g. geo-location
    # analysis.
    #
    #ns_param skipnonpublic  false
}

#---------------------------------------------------------------------
# Definition of NaviServer servers (add more servers, when true
# NaviServer virtual hosting should be used).
# ---------------------------------------------------------------------
ns_section ns/servers {
    ns_param $server $serverprettyname
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

#---------------------------------------------------------------------
# Configuration for plain HTTP interface  -- module nssock
#---------------------------------------------------------------------
if {[info exists httpport] && $httpport ne ""} {
    #
    # We have an "httpport" configured, so load and configure the
    # module "nssock" as a global server module with the name "http".
    #
    ns_section ns/modules {
        ns_param http ${bindir}/nssock
    }

    ns_section ns/module/http {
        ns_param	defaultserver	$server
        ns_param	address		$ipaddress
        ns_param	hostname	[lindex $hostname 0]
        ns_param	port		$httpport                  ;# default 80
        ns_param	maxinput	$max_file_upload_size      ;# 1MB, maximum size for inputs (uploads)
        ns_param	recvwait	$max_file_upload_duration  ;# 30s, timeout for receive operations
        # ns_param	maxline		8192	;# 8192, max size of a header line
        # ns_param	maxheaders	128	;# 128, max number of header lines
        # ns_param	uploadpath	/tmp	;# directory for uploads
        # ns_param	maxqueuesize	256	;# 1024, maximum size of the queue of preprocessed requests
        # ns_param	backlog		256	;# 256, backlog for listen operations
        # ns_param	acceptsize	10	;# backlog; Maximum number of requests accepted at once.
        # ns_param      sockacceptlog   3       ;# ns/param sockacceptlog; report, when one accept operation
                                                 # receives more than this threshold number of sockets
        # ns_param	deferaccept     true    ;# false, Performance optimization, may cause recvwait to be ignored
        # ns_param	bufsize		16kB	;# 16kB, buffersize
        # ns_param	readahead	16kB	;# value of bufsize, size of readahead for requests
        # ns_param	sendwait	30s	;# 30s, timeout for send operations
        # ns_param	closewait	2s	;# 2s, timeout for close on socket
        # ns_param	keepwait	2s	;# 5s, timeout for keep-alive
        # ns_param	nodelay         false   ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        # ns_param	keepalivemaxuploadsize    500kB  ;# 0, don't allow keep-alive for upload content larger than this
        # ns_param	keepalivemaxdownloadsize  1MB    ;# 0, don't allow keep-alive for download content larger than this
        # ns_param	spoolerthreads	1	;# 0, number of upload spooler threads
        ns_param	maxupload	100kB	;# 0, when specified, spool uploads larger than this value to a temp file
        ns_param	writerthreads	2	;# 0, number of writer threads
        ns_param	writersize	1kB	;# 1MB, use writer threads for files larger than this value
        # ns_param	writerbufsize	8kB	;# 8kB, buffer size for writer threads
        # ns_param	writerstreaming	true	;# false;  activate writer for streaming HTML output (when using ns_write)

        #
        # Options for port reuse (see https://lwn.net/Articles/542629/)
        # These options require proper OS support.
        #
        # ns_param  reuseport       true    ;# false;  normally not needed to be set, set by driverthreads when necessary
        # ns_param	driverthreads	2	;# 1; use multiple driver threads; activates "reuseport"

        #
        # Extra driver-specific response headers fields to be added for
        # every request.
        #
        ns_param    extraheaders    $http_extraheaders
    }
    #
    # Define, which "host" (as supplied by the "host:" header field)
    # accepted over this driver should be associated with which
    # server. The variable hostname can contain multiple hostnames
    # (domain names) which are registered below.
    #
    ns_section ns/module/http/servers {
        foreach domainname $hostname {
            ns_param $server $domainname
        }
        foreach address $ipaddress {
            ns_param $server $address
        }
        if {[info exists ::docker::containerMapping] && [dict exists $::docker::containerMapping $httpport/tcp]} {
            set __host [dict get $::docker::containerMapping $httpport/tcp host]
            set __port [dict get $::docker::containerMapping $httpport/tcp port]
            ns_log notice "added white-listed address '${__host}:${__port}' for server $server on HTTP driver"
            ns_param $server ${__host}:${__port}
        }
    }
}

#---------------------------------------------------------------------
# Configuration for HTTPS interface (SSL/TLS) -- module nsssl
#---------------------------------------------------------------------

if {[info exists httpsport] && $httpsport ne ""} {
    #
    # We have an "httpsport" configured, so configure this module.
    #
    #
    # We have an "httpsport" configured, so load and configure the
    # module "nsssl" as a global server module with the name "https".
    #
    ns_section ns/modules {
        ns_param https  ${bindir}/nsssl
    }

    ns_section ns/module/https {
        ns_param defaultserver	$server
        ns_param address	$ipaddress
        ns_param port		$httpsport
        ns_param hostname	$hostname
        ns_param ciphers	"ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"
        #ns_param ciphersuites  "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"
        ns_param protocols	"!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"
        ns_param certificate	$certificate
        ns_param vhostcertificates $vhostcertificates ;# directory for vhost certificates of the default server
        ns_param verify		0
        ns_param writerthreads	2
        ns_param writersize	1kB
        ns_param writerbufsize	16kB	;# 8kB, buffer size for writer threads
        # ns_param writerstreaming	true	;# false
        # ns_param spoolerthreads	1	;# 0, number of upload spooler threads
        # ns_param maxupload	100kB	;# 0, when specified, spool uploads larger than this value to a temp file
        # ns_param backlog	256	;# 256, backlog for listen operations
        # ns_param acceptsize	10	;# backlog; Maximum number of requests accepted at once.
        # ns_param sockacceptlog 3      ;# ns/param sockacceptlog; report, when one accept operation
                                         # receives more than this threshold number of sockets
        # ns_param deferaccept	true    ;# false, Performance optimization
        # ns_param nodelay	false   ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        ns_param maxinput	$max_file_upload_size   ;# Maximum file size for uploads in bytes
        ns_param recvwait	$max_file_upload_duration  ;# 30s, timeout for receive operations
        ns_param extraheaders	$https_extraheaders
        ns_param OCSPstapling   on        ;# off; activate OCSP stapling
        # ns_param OCSPstaplingVerbose  on ;# off; make OCSP stapling more verbose
    }
    #
    # Define, which "host" (as supplied by the "host:" header field)
    # accepted over this driver should be associated with which
    # server. This parameter is for virtual servers. Here we just
    # register the $hostname and the $address (in case, the server is
    # addressed via its IP address).
    #
    ns_section ns/module/https/servers {
        foreach domainname $hostname {
            ns_param $server $domainname
        }
        foreach address $ipaddress {
            ns_param $server $address
        }
        if {[info exists ::docker::containerMapping] && [dict exists $::docker::containerMapping $httpsport/tcp]} {
            set __host [dict get $::docker::containerMapping $httpsport/tcp host]
            set __port [dict get $::docker::containerMapping $httpsport/tcp port]
            ns_log notice "added white-listed address '${__host}:${__port}' for server $server on HTTPS driver"
            ns_param $server ${__host}:${__port}
        }
    }
}


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
    #ns_param        gzip_static         true       ;# check for static gzip; default: false
    #ns_param        gzip_refresh        true       ;# refresh stale .gz files on the fly using ::ns_gzipfile
    #ns_param        gzip_cmd            "/usr/bin/gzip -9"  ;# use for re-compressing
    #ns_param        minify_css_cmd      "/usr/bin/yui-compressor --type css"
    #ns_param        minify_js_cmd       "/usr/bin/yui-compressor --type js"
    #ns_param        brotli_static       true       ;# check for static brotli files; default: false
    #ns_param        brotli_refresh      true       ;# refresh stale .br files on the fly using ::ns_brotlifile
    #ns_param        brotli_cmd          "/usr/bin/brotli -f -Z"  ;# use for re-compressing
}

#---------------------------------------------------------------------
#
# Server-level configuration
#
#  There is only one server in NaviServer, but this is helpful when multiple
#  servers share the same configuration file.  This file assumes that only
#  one server is in use so it is set at the top in the "server" Tcl variable
#  Other host-specific values are set up above as Tcl variables, too.
#
#---------------------------------------------------------------------
#
# Server parameters
#
ns_section ns/server/$server {
    #
    # Scaling and Tuning Options
    #
    # ns_param	maxconnections	100      ;# 100; number of allocated connection structures
    ns_param    rejectoverrun   true     ;# false (send 503 when queue overruns)
    #ns_param   retryafter      5s       ;# time for Retry-After in 503 cases
    #ns_param   filterrwlocks   false    ;# default: true

    # ns_param	maxthreads	10       ;# 10; maximal number of connection threads
    ns_param	minthreads	2        ;# 1; minimal number of connection threads

    #ns_param	connsperthread	1000     ;# 10000; number of connections (requests) handled per thread
    ;# Setting connsperthread to > 0 will cause the thread to
    ;# graciously exit, after processing that many requests, thus
    ;# initiating kind-of Tcl-level garbage collection.

    # ns_param	threadtimeout	2m       ;# 2m; timeout for idle threads.
    ;# In case, minthreads < maxthreads, threads are shutdown after
    ;# this idle time until minthreads are reached.

    # ns_param	lowwatermark	10       ;# 10; create additional threads above this queue-full percentage
    ns_param	highwatermark	100      ;# 80; allow concurrent creates above this queue-is percentage
                                          # 100 means to disable concurrent creates
    #ns_param    connectionratelimit 200 ;# 0; limit rate per connection to this amount (KB/s); 0 means unlimited
    #ns_param    poolratelimit   200     ;# 0; limit rate for pool to this amount (KB/s); 0 means unlimited

    # Compress response character data: ns_return, ADP etc.
    #
    ns_param	compressenable	on       ;# false, use "ns_conn compress" to override
    # ns_param	compresslevel	4        ;# 4, 1-9 where 9 is high compression, high overhead
    # ns_param	compressminsize	512      ;# Compress responses larger than this
    # ns_param	compresspreinit true     ;# false, if true then initialize and allocate buffers at startup

    # Enable nicer directory listing (as handled by the OpenACS request processor)
    # ns_param	directorylisting	fancy	;# Can be simple or fancy

    #
    # Configuration of replies
    #
    # ns_param	realm		yourrealm	;# Default realm for Basic authentication
    # ns_param	noticedetail	false		;# true, return detail information in server reply
    # ns_param	noticeadp	returnnotice.adp ;# returnnotice.adp; ADP file for ns_returnnotice.
    # ns_param	errorminsize	0		;# 514, fill-up reply to at least specified bytes (for ?early? MSIE)
    # ns_param	headercase	preserve	;# preserve, might be "tolower" or "toupper"
    # ns_param	checkmodifiedsince	false	;# true, check modified-since before returning files from cache. Disable for speedup

    #
    # Extra server-specific response headers fields to be added for
    # every response on this server
    #
    #ns_param    extraheaders    {...}
}

########################################################################
# Connection thread pools.
#
#  Per default, NaviServer uses a "default" connection pool, which
#  needs no extra configuration.  Optionally, multiple connection
#  thread pools can be configured using the following parameters.
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
#
# In order to define thread pools, do the following:
#
#  1. Add pool names to "ns/server/$server/pools"
#  2. Configure pools with the noted parameters
#  3. Map method/URL combinations for these pools
#
#  All unmapped method/URL's will go to the default server pool of the
#  server.
#
########################################################################

ns_section ns/server/$server/pools {
    #
    # To activate connection thread pools, uncomment one of the
    # following lines and/or add other pools.

    #ns_param   monitor	"Monitoring actions to check healthiness of the system"
    #ns_param   fast	"Fast requests, e.g. less than 10ms"
    #ns_param   slow    "Slow lane pool, for request remapping"
}

ns_section ns/server/$server/pool/monitor {
    ns_param   minthreads 2
    ns_param   maxthreads 2

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
    ns_param   minthreads 2
    ns_param   maxthreads 2
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
    ns_param   minthreads 2
    ns_param   maxthreads 5
    ns_param   maxconnections 600
    ns_param   rejectoverrun true
}


#---------------------------------------------------------------------
# Special HTTP pages
#---------------------------------------------------------------------
ns_section ns/server/$server/redirects {
    ns_param   404 /shared/404
    ns_param   403 /shared/403
    ns_param   503 /shared/503
    ns_param   500 /shared/500
}

#---------------------------------------------------------------------
# ADP (AOLserver Dynamic Page) configuration
#---------------------------------------------------------------------
ns_section ns/server/$server/adp {
    ns_param	enabledebug	$debug
    ns_param	map		/*.adp		;# Extensions to parse as ADP's
    # ns_param	map		"/*.html"	;# Any extension can be mapped
    #
    # ns_param	cache		true		;# false, enable ADP caching
    # ns_param	cachesize	10MB		;# 5MB, size of ADP cache
    # ns_param	bufsize		5MB		;# 1MB, size of ADP buffer
    #
    # ns_param	trace		true		;# false, trace execution of adp scripts
    # ns_param	tracesize	100		;# 40, max number of entries in trace
    #
    # ns_param	stream		true		;# false, enable ADP streaming
    # ns_param	enableexpire	true		;# false, set "Expires: now" on all ADP's
    # ns_param	safeeval	true		;# false, disable inline scripts
    # ns_param	singlescript	true		;# false, collapse Tcl blocks to a single Tcl script
    # ns_param	detailerror	false		;# true,  include connection info in error backtrace
    # ns_param	stricterror	true		;# false, interrupt execution on any error
    # ns_param	displayerror	true		;# false, include error message in output
    # ns_param	trimspace	true		;# false, trim whitespace from output buffer
    # ns_param	autoabort	false		;# true,  failure to flush a buffer (e.g. closed HTTP connection) generates an ADP exception
    #
    # ns_param	errorpage	/.../errorpage.adp	;# page for returning errors
    # ns_param	startpage	/.../startpage.adp	;# file to be run for every adp request; should include "ns_adp_include [ns_adp_argv 0]"
    # ns_param	debuginit	some-proc		;# ns_adp_debuginit, proc to be executed on debug init
    #
}

ns_section ns/server/$server/adp/parsers {
    ns_param	fancy		".adp"
}

#
# Tcl Configuration
#
ns_section ns/server/$server/tcl {
    ns_param	library		${serverroot}/tcl
    ns_param	debug		$debug
    # ns_param	nsvbuckets	16       ;# default: 8
    # ns_param	nsvrwlocks      false    ;# default: true
}

ns_section ns/server/$server/fastpath {
    ns_param	serverdir	${homedir}
    ns_param	pagedir		${pageroot}
    #
    # Directory listing options
    #
    # ns_param	directoryfile     "index.adp index.tcl index.html index.htm"
    # ns_param	directoryadp      $pageroot/dirlist.adp ;# default ""
    # ns_param	directoryproc     _ns_dirlist           ;# default "_ns_dirlist"
    # ns_param	directorylisting  fancy ;# default "simple"; can be "simple",
    #                                   ;# "fancy" or "none"; parameter for _ns_dirlist
    # ns_param	hidedotfiles      true  ;# default false; parameter for _ns_dirlist
    #
}

#---------------------------------------------------------------------
# HTTP client (ns_http) configuration
#---------------------------------------------------------------------
ns_section ns/server/$server/httpclient {
    #
    # Set default keep-alive timeout for outgoing ns_http requests
    #
    #ns_param	keepalive       5s       ;# default: 0s

    #
    # Configure log file for outgoing ns_http requests
    #
    ns_param	logging		on       ;# default: off
    ns_param	logfile		${logdir}/httpclient.log
    ns_param	logrollfmt	%Y-%m-%d ;# format appended to log filename
    #ns_param	logmaxbackup	100      ;# 10, max number of backup log files
    #ns_param	logroll		true     ;# true, should server log files automatically
    #ns_param	logrollonsignal	true     ;# false, perform roll on a sighup
    #ns_param	logrollhour	0        ;# 0, specify at which hour to roll
}

#---------------------------------------------------------------------
# OpenACS specific settings (per server)
#---------------------------------------------------------------------
#
# Define/override OpenACS kernel parameter for $server
#
ns_section ns/server/$server/acs {
    #
    # Provide optionally a different cookie namespace (used for
    # prefixing OpenACS cookies). This is important when e.g.
    # multiple servers are running on different ports of the same
    # host, but should not share cookies.
    #
    ns_param CookieNamespace $CookieNamespace
    #
    # Define a mapping between MIME types and CSP rules for static
    # files. The mapping is of the form of a Tcl dict. The mapping is
    # used e.g. in "security::csp::add_static_resource_header".
    #
    ns_param StaticCSP {
        image/svg+xml "script-src 'none'"
    }

    #
    # The configuration file might contain white-listed hosts to be
    # accepted as value of the host header field (typically domain
    # name with optional port). Validation is needed, e.g., to avoid
    # accepting spoofed host header fields causing hijacking to a
    # different web site via redirects. Setting these values is
    # typically necessary when running behind a proxy server and/or in
    # containerized environments, where the host header field does not
    # match any driver configuration.
    #
    # Watch out for "ignore untrusted host header field" in the error
    # log for cases, where a value might be missing.
    #
    ns_param whitelistedHosts {}

    #
    # The following option should causes on acs-admin/server-restart
    # the usage of "ns_shutdown -restart" instead of plain
    # "ns_shutdown".  This seems to be required in current Windows
    # installations. Default is 0.
    #
    # ns_param NsShutdownWithNonZeroExitCode 1

    #
    # Should deprecated log be used? Use value of 1 on legacy
    # sites. Default is 1.
    #
    # ns_param WithDeprecatedCode 0

    #
    # Should user_ids be included in log files? Some sensitive sites
    # do not allow this. Default is 0.
    #
    # ns_param LogIncludeUserId 1
    #

    #
    # Cluster secret for intra-cluster communications. Clustering will
    # not be enabled if no value is provided.
    #
    ns_param clusterSecret $clusterSecret

    #
    # "parameterSecret" is needed for signed query and form parameters
    #
    ns_param parameterSecret             $parameterSecret
}


# Define/override OpenACS package parameters in section
# ending with /acs/PACKAGENAME
#
# Provide tailored sizes for the site node cache in acs-tcl:
#
ns_section ns/server/$server/acs/acs-tcl {
    # ns_param SiteNodesCacheSize        2000000
    # ns_param SiteNodesIdCacheSize       100000
    # ns_param SiteNodesChildenCacheSize  100000
    # ns_param SiteNodesPrefetch  {/file /changelogs /munin}
    # ns_param UserInfoCacheSize          2000000
}

#
# Set for all package instances of acs-mail-lite the
# EmailDeliveryMode. Setting this to "log" is useful for developer
# instances.
#
ns_section ns/server/$server/acs/acs-mail-lite {
    # ns_param EmailDeliveryMode log
}

#
# API browser configuration: setting IncludeCallingInfo to "true" is
# useful mostly for developer instances.
#
ns_section ns/server/$server/acs/acs-api-browser {
    # ns_param IncludeCallingInfo true
}

#---------------------------------------------------------------------
# WebDAV Support (optional, requires oacs-dav package to be installed
#---------------------------------------------------------------------
ns_section ns/server/$server/tdav {
    ns_param	propdir            ${serverroot}/data/dav/properties
    ns_param	lockdir            ${serverroot}/data/dav/locks
    ns_param	defaultlocktimeout 300
}

ns_section ns/server/$server/tdav/shares {
    ns_param	share1		"OpenACS"
    # ns_param	share2		"Share 2 description"
}

ns_section ns/server/$server/tdav/share/share1 {
    ns_param	uri		"/dav/*"
    # all WebDAV options
    ns_param	options		"OPTIONS COPY GET PUT MOVE DELETE HEAD MKCOL POST PROPFIND PROPPATCH LOCK UNLOCK"
}

#ns_section ns/server/$server/tdav/share/share2 {
# ns_param	uri "/share2/path/*"
# read-only WebDAV options
# ns_param options "OPTIONS COPY GET HEAD MKCOL POST PROPFIND PROPPATCH"
#}


#---------------------------------------------------------------------
# Access log -- nslog
#---------------------------------------------------------------------
ns_section ns/server/$server/module/nslog {
    #
    # General parameters for access.log
    #
    ns_param	file			${logdir}/access.log
    # ns_param	maxbuffer		100	;# 0, number of logfile entries to keep in memory before flushing to disk
    #
    # Control what to log
    #
    # ns_param	suppressquery	true	;# false, suppress query portion in log entry
    # ns_param	logreqtime	true	;# false, include time to service the request
    ns_param	logpartialtimes	true	;# false, include high-res start time and partial request durations (accept, queue, filter, run)
    ns_param    logthreadname   true    ;# default: false; include thread name for linking with error.log
    # ns_param	formattedtime	true	;# true, timestamps formatted or in secs (unix time)
    # ns_param	logcombined	true	;# true, Log in NSCA Combined Log Format (referer, user-agent)
    ns_param	checkforproxy	$reverseproxymode ;# false, check for proxy header (X-Forwarded-For)
    ns_param	masklogaddr     true    ;# false, mask IP address in log file for GDPR (like anonip IP anonymizer)
    ns_param	maskipv4        255.255.255.0  ;# mask for IPv4 addresses
    ns_param	maskipv6        ff:ff:ff:ff::  ;# mask for IPv6 addresses

    #
    # Add extra entries to the access log via specifying a Tcl
    # list of request header fields in "extendedheaders"
    #
    if {[ns_config "ns/server/$server/acs" LogIncludeUserId 0]} {
        ns_param   extendedheaders    "X-User-Id"
    }

    #
    #
    # Control log file rolling
    #
    # ns_param	maxbackup	100	;# 10, max number of backup log files
    # ns_param	rolllog		true	;# true, should server log files automatically
    # ns_param	rollhour	0	;# 0, specify at which hour to roll
    # ns_param	rollonsignal	true	;# false, perform roll on a sighup
    ns_param	rollfmt		%Y-%m-%d ;# format appended to log filename
}

#---------------------------------------------------------------------
#
# CGI interface -- nscgi, if you have legacy stuff. Tcl or ADP files
# inside NaviServer are vastly superior to CGIs.
#
#---------------------------------------------------------------------
# ns_section ns/server/$server/modules {
#     ns_param	nscgi ${bindir}/nscgi
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
#
# PAM authentication
#
#---------------------------------------------------------------------
# ns_section ns/server/$server/modules {
#     ns_param	nspam ${bindir}/nspam
# }
# ns_section ns/server/$server/module/nspam {
#     ns_param	PamDomain "pam_domain"
# }

#---------------------------------------------------------------------
#
# Dbms drivers:
#
# Make sure the drivers are compiled and put it in $bindir.
#
#---------------------------------------------------------------------
ns_section ns/db/drivers {

    if { $dbms eq "oracle" } {
        ns_param	nsoracle       ${bindir}/nsoracle
    } else {
        ns_param	postgres       ${bindir}/nsdbpg
        #
        ns_logctl severity "Debug(sql)" -color blue $verboseSQL
    }

    if { $dbms eq "oracle" } {
        ns_section ns/db/driver/nsoracle
        ns_param	maxStringLogLength -1
        ns_param	LobBufferSize      32768
    } else {
        ns_section ns/db/driver/postgres
        # Set this parameter, when "psql" is not on your path (OpenACS specific)
        # ns_param	pgbin	"/usr/local/pg960/bin/"
    }
}

# Database Pools: This is how NaviServer "talks" to the RDBMS. You
# need three for OpenACS, named here pool1, pool2 and pool3. Most
# queries use to first pool, nested queries (i.e. in a db_foreach,
# which is actually not recommended) use pool2 and so on. Make sure to
# set the "db_*" the variables with actual values on top of this file.
#
# NaviServer can have different pools connecting to different databases
# and even different database servers.  See
#
#     http://openacs.org/doc/tutorial-second-database
#
ns_section ns/server/$server/db {
    ns_param	pools              pool1,pool2,pool3
    ns_param	defaultpool        pool1
}
ns_section ns/db/pools {
    ns_param	pool1              "Pool 1"
    ns_param	pool2              "Pool 2"
    ns_param	pool3              "Pool 3"
}

ns_section ns/db/pool/pool1 {
    # ns_param	maxidle            0     ;# time interval for shut-down of idle connections; default: 5m
    # ns_param	maxopen            0     ;# time interval for maximum time of open connections; default: 60m
    # ns_param  checkinterval      5m    ;# check pools for stale handles in this interval
    ns_param	connections        15
    ns_param    LogMinDuration     10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param	logsqlerrors       $debug
    ns_param	datasource         $datasource
    ns_param	user               $db_user
    ns_param	password           $db_password
    if { $dbms eq "oracle" } {
        ns_param	driver             nsoracle
    } else {
        ns_param	driver             postgres
    }
}

ns_section ns/db/pool/pool2 {
    # ns_param	maxidle            0
    # ns_param	maxopen            0
    # ns_param  checkinterval      5m    ;# check pools for stale handles in this interval
    ns_param	connections        5
    ns_param    LogMinDuration     10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param	logsqlerrors       $debug
    ns_param	datasource         $datasource
    ns_param	user               $db_user
    ns_param	password           $db_password
    if { $dbms eq "oracle" } {
        ns_param	driver             nsoracle
    } else {
        ns_param	driver             postgres
    }
}

ns_section ns/db/pool/pool3 {
    # ns_param	maxidle            0
    # ns_param	maxopen            0
    # ns_param  checkinterval      5m    ;# check pools for stale handles in this interval
    ns_param	connections        5
    # ns_param  LogMinDuration     0ms  ;# when SQL logging is on, log only statements above this duration
    ns_param	logsqlerrors       $debug
    ns_param	datasource         $datasource
    ns_param	user               $db_user
    ns_param	password           $db_password
    if { $dbms eq "oracle" } {
        ns_param	driver             nsoracle
    } else {
        ns_param	driver             postgres
    }
}


#---------------------------------------------------------------------
# Which modules should be loaded for $server?  Missing modules break
# the server, so don't uncomment modules unless they have been
# installed.

ns_section ns/server/$server/modules {
    ns_param nslog ${bindir}/nslog
    ns_param nsdb ${bindir}/nsdb
    ns_param nsproxy ${bindir}/nsproxy

    #
    # Determine, if libthread is installed. First check for a version
    # having the "-ns" suffix. If this does not exist, check for a
    # legacy version without it.
    #
    set libthread [lindex [lsort [glob -nocomplain $homedir/lib/thread*/libthread-ns*[info sharedlibextension]]] end]
    if {$libthread eq ""} {
        set libthread [lindex [lsort [glob -nocomplain $homedir/lib/thread*/lib*thread*[info sharedlibextension]]] end]
    }
    if {$libthread eq ""} {
        ns_log notice "No Tcl thread library installed in $homedir/lib/"
    } else {
        ns_param	libthread $libthread
        ns_log notice "Use Tcl thread library $libthread"
    }

    # LDAP authentication
    # ns_param	nsldap             ${bindir}/nsldap

    # These modules aren't used in standard OpenACS installs
    # ns_param	nsperm             ${bindir}/nsperm
}

#---------------------------------------------------------------------
# Example configuration for the NaviServer Control Port (nscp)
#
# To enable:
#
# 1. Define an address and port to listen on. For security reasons
#    listening on any ipaddress other than the loopback address
#    (127.0.0.1 or ::1) is not recommended. For this script, it can be
#    activated by setting the variable "nscpport" to a valid port used
#    for listening (e.g. 9999).
#
# 2. Decide whether you wish to enable features such as password
#    echoing at login time, and command logging.
#
# 3. Add a list of authorized users and passwords. The entries
#    take the following format:
#
#    <user>:<encryptedPassword>:
#
#    You can use the ns_crypt Tcl command to generate an encrypted
#    password. The ns_crypt command uses the same algorithm as the
#    Unix crypt(3) command. You could also use passwords from the
#    /etc/passwd file.
#
#    The first two characters of the password are the salt - they can be
#    anything since the salt is used to simply introduce disorder into
#    the encoding algorithm.
#
#    ns_crypt <key> <salt>
#    ns_crypt x t2
#
#    The configuration example below adds the user "nsadmin" with a
#    password of "x".
#
# 4. Make sure the "nscp" module is loaded in the modules section.
#
ns_section "ns/server/${server}/module/nscp" {
    ns_param address 127.0.0.1
    ns_param port $nscpport
    ns_param echopassword 1
    ns_param cpcmdlogging 1
}

ns_section "ns/server/${server}/module/nscp/users" {
    ns_param user "nsadmin:t2GqvvaiIUbF2:"
}

ns_section "ns/server/${server}/modules" {
    if {$nscpport ne ""} {ns_param nscp nscp}
}

#
# nsproxy configuration
#
ns_section ns/server/$server/module/nsproxy {
    # ns_param	maxworkers         8
    # ns_param	sendtimeout        5s
    # ns_param	recvtimeout        5s
    # ns_param	waittimeout        100ms
    # ns_param	idletimeout        5m
    # ns_param	logminduration     1s
}

#
# nsstats configuration (global module)
#
# When installed under acs-subsite/www/admin/nsstats.tcl it is due to
# its /admin/ location safe from public access.
#
ns_section ns/module/nsstats {
    ns_param enabled  1
    ns_param user     ""
    ns_param password ""
    ns_param bglocks  {oacs:sched_procs}
}


#
# Sample letsencrypt configuration.
#
# To use this, it is necessary to install the NaviServer letsencrypt
# module first, uncomment the two "ns_param" lines, and provide your
# desired domain names.
#
ns_section "ns/server/${server}/modules" {
    #ns_param letsencrypt tcl
}
ns_section ns/server/${server}/module/letsencrypt {

    # Provide one or more domain names (latter for multi-domain SAN
    # certificates). These values are a default in case the domains
    # are not provided by other means (e.g. "letsencrypt.tcl").  In
    # case multiple NaviServer virtual hosts are in used, this
    # definition must be on the ${server}, which is used for
    # obtaining updates (e.g. main site) although it retrieves a
    # certificate for many subsites.

    #ns_param domains { openacs.org openacs.net fisheye.openacs.org cvs.openacs.org }
}

#
# Sample configuration for the nssmtpd module.
#
# To use this, it is necessary to install the NaviServer nssmtpd
# module first, and to provide a nonempty "smtpdport" below, and set
# the package parameter "EmailDeliveryMode" in the acs-mail-lite
# package to "nssmtpd". See: https://openacs.org/xowiki/outgoing_email
#
ns_section "ns/server/${server}/module/nssmtpd" {
    ns_param port $smtpdport
    ns_param address 127.0.0.1
    ns_param relay localhost:25
    ns_param spamd localhost
    ns_param initproc smtpd::init
    ns_param rcptproc smtpd::rcpt
    ns_param dataproc smtpd::data
    ns_param errorproc smtpd::error
    ns_param relaydomains "localhost"
    ns_param localdomains "localhost"
    #
    # Next section is for STARTTLS functionality:
    #
    #ns_param certificate "pathToYourCertificateChainFile.pem"
    #ns_param cafile ""
    #ns_param capath ""
    #ns_param ciphers "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"

    ns_param logging on ;# default: off
    ns_param logfile ${logdir}/smtpsend.log
    ns_param logrollfmt %Y-%m-%d ;# format appended to log filename
    #ns_param logmaxbackup 100 ;# 10, max number of backup log files
    #ns_param logroll true ;# true, should server log files automatically
    #ns_param logrollonsignal true ;# false, perform roll on a sighup
    #ns_param logrollhour 0 ;# 0, specify at which hour to roll
}
ns_section ns/server/${server}/modules {
    if {$smtpdport ne ""} {ns_param nssmtpd nssmtpd}
}



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
