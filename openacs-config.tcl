######################################################################
#
# Config parameter for an OpenACS site using NaviServer.
#
# These default settings will only work in limited circumstances.
# Two servers with default settings cannot run on the same host
#
######################################################################
ns_log notice "nsd.tcl: starting to read config file..."

#---------------------------------------------------------------------
# Change the HTTP and HTTPS port to e.g. 80 and 443 for production use.
set httpport		8000

#
# Setting the HTTPS port to 0 means to active the https driver for
# ns_http, but do not listen on this port.
#
#set httpsport		0
#set httpsport		8443

# The hostname and address should be set to actual values.
# setting the address to 0.0.0.0 means NaviServer listens on all interfaces
set hostname		localhost
set address_v4		127.0.0.1  ;# listen on loopback via IPv4
#set address_v4		0.0.0.0    ;# listen on all IPv4 addresses
#set address_v6		::1        ;# listen on loopback via IPv6
#set address_v6		::0        ;# listen on all IPv6 addresses

# Note: If port is privileged (usually < 1024), OpenACS must be
# started by root, and the run script must contain the flag
# '-b address:port' which matches the address and port
# as specified above.

set server		"openacs"
set servername		"New OpenACS Installation - Development"

set serverroot		/var/www/$server
set logroot		$serverroot/log/

set homedir		/usr/local/ns
set bindir		$homedir/bin

# Are we running behind a proxy?
set proxy_mode		false

#---------------------------------------------------------------------
# Which database do you want? PostgreSQL or Oracle?
set database              postgres
set db_name               $server

if { $database eq "oracle" } {
    set db_password           "mysitepassword"
} else {
    set db_host               localhost
    set db_port               ""
    set db_user               $server
}

#---------------------------------------------------------------------
# If debug is false, all debugging will be turned off.
set debug false
set dev   false
set verboseSQL false

set max_file_upload_mb        20
set max_file_upload_min        5

#---------------------------------------------------------------------
# Set environment variables HOME and LANG. HOME is needed since
# otherwise some programs called via exec might try to write into the
# root home directory.
#
set env(HOME) $homedir
set env(LANG) en_US.UTF-8

#---------------------------------------------------------------------
# Set headers that should be included in every response from the
# server.
#
set nssock_extraheaders {
    X-Frame-Options            "SAMEORIGIN"
    X-Content-Type-Options     "nosniff"
    X-XSS-Protection           "1; mode=block"
    Referrer-Policy            "strict-origin"
}

set nsssl_extraheaders {
    Strict-Transport-Security "max-age=31536000; includeSubDomains"
}
append nsssl_extraheaders $nssock_extraheaders

######################################################################
#
# End of instance-specific settings
#
# Nothing below this point need be changed in a default install.
#
######################################################################


ns_logctl severity "Debug(ns:driver)" $debug

set addresses {}
if {[info exists address_v4]} {lappend addresses $address_v4}
if {[info exists address_v6]} {lappend addresses $address_v6}

if {[llength $addresses] == 0} {
    ns_log error "Either an IPv4 or IPv6 address must be specified"
    exit
}

#---------------------------------------------------------------------
#
# NaviServer's directories. Auto-configurable.
#
#---------------------------------------------------------------------
# Where are your pages going to live ?
set pageroot                  ${serverroot}/www
set directoryfile             "index.tcl index.adp index.html index.htm"

#---------------------------------------------------------------------
# Global server parameters
#---------------------------------------------------------------------
ns_section ns/parameters {
    ns_param	serverlog	${logroot}/error.log
    ns_param	pidfile		${logroot}/nsd.pid
    ns_param	home		$homedir
    ns_param	debug		$debug

    # Define optionally the tmpdir. If not specified, the
    # environment variable TMPDIR is used. If that is not
    # specified either, a system specific constant us used
    # (compile time macro P_tmpdir)
    #
    # ns_param        tmpdir    c:/tmp

    # Timeout for shutdown in seconds to let existing connections and
    # background jobs finish.  When this time limit is exceeded the
    # server shuts down immediately.
    # ns_param    shutdowntimeout 20s      ;# 20s is the default

    #
    # Configuration of error.log
    #
    # Rolling of logfile:
    ns_param	logroll		on
    ns_param	logmaxbackup	100      ;# 10 is default
    ns_param	logrollfmt	%Y-%m-%d ;# format appended to serverlog file name when rolled
    #
    # Format of log entries:
    # ns_param  logusec         true     ;# add timestamps in microsecond (usec) resolution (default: false)
    # ns_param  logusecdiff     true     ;# add timestamp diffs since in microsecond (usec) resolution (default: false)
    ns_param	logcolorize	true     ;# colorize log file with ANSI colors (default: false)
    ns_param	logprefixcolor	green    ;# black, red, green, yellow, blue, magenta, cyan, gray, default
    # ns_param  logprefixintensity normal;# bright or normal
    #
    # Severities to be logged (can be controlled at runtime via ns_logctl)
    ns_param	logdebug	$debug    ;# debug messages
    ns_param	logdev		$dev      ;# development message
    ns_param    lognotice       true      ;# informational messages
    #ns_param   sanitizelogfiles 2        ;# default: 2; 0: none, 1: full, 2: human-friendly

    # ns_param	mailhost	localhost
    # ns_param	jobsperthread	0
    # ns_param	jobtimeout	5m
    # ns_param	schedsperthread	0

    # Write asynchronously to log files (access log and error log)
    # ns_param	asynclogwriter	true		;# false

    #ns_param       mutexlocktrace       true   ;# default false; print durations of long mutex calls to stderr

    # Reject output operations on already closed connections (e.g. subsequent ns_return statements)
    #ns_param       rejectalreadyclosedconn false ;# default: true

    # Allow concurrent create operations of Tcl interpreters.
    # Versions up to at least Tcl 8.5 are known that these might
    # crash in case two threads create interpreters at the same
    # time. These crashes were hard to reproduce, but serializing
    # interpreter creation helped. Probably it is possible to
    # allow concurrent interpreter create operations in Tcl 8.6.
    #ns_param        concurrentinterpcreate true   ;# default: false

    # Enforce sequential thread initialization. This is not really
    # desirably in general, but might be useful for hunting strange
    # crashes or for debugging with valgrind.
    # ns_param	tclinitlock	true	       ;# default: false

    #
    # Encoding settings (see http://dqd.com/~mayoff/encoding-doc.html)
    #
    # ns_param	HackContentType	1

    # NaviServer's defaults charsets are all utf-8.  Although the
    # default charset is utf-8, set the parameter "OutputCharset"
    # here, since otherwise OpenACS uses in the meta-tags the charset
    # from [ad_conn charset], which is taken from the db and
    # per-default ISO-8859-1.
    ns_param	OutputCharset	utf-8
    # ns_param	URLCharset	utf-8

    #
    # DNS configuration parameters
    #
    ns_param dnscache true          ;# default: true
    ns_param dnswaittimeout 5s      ;# time for waiting for a DNS reply; default: 5s
    ns_param dnscachetimeout 1h     ;# time to keep entries in cache; default: 1h
    ns_param dnscachemaxsize 500kB  ;# max size of DNS cache in memory units; default: 500kB

    # Running behind proxy? Used by OpenACS...
    ns_param ReverseProxyMode	$proxy_mode
}

#---------------------------------------------------------------------
# Definition of NaviServer servers (add more, when true NaviServer
# virtual hosting should be used).
#---------------------------------------------------------------------
ns_section ns/servers {
    ns_param $server $servername
}


#---------------------------------------------------------------------
# Global server modules
#---------------------------------------------------------------------
ns_section "ns/modules" {
    #
    # Load networking modules named "nssock" and/or "nsssl" depending
    # on existence of Tcl variables "httpport" and "httpsport".
    #
    if {[info exists httpport]}  { ns_param nssock ${bindir}/nssock }
    if {[info exists httpsport]} { ns_param nsssl  ${bindir}/nsssl }
}

#---------------------------------------------------------------------
# Configuration for plain HTTP interface  -- module nssock
#---------------------------------------------------------------------
if {[info exists httpport]} {
    #
    # We have an "httpport" configured, so configure this module.
    #
    ns_section ns/module/nssock {
	ns_param	defaultserver	$server
	ns_param	address		$addresses
	ns_param	hostname	$hostname
	ns_param	port		$httpport                ;# default 80
	ns_param	maxinput	${max_file_upload_mb}MB  ;# 1MB, maximum size for inputs (uploads)
	ns_param	recvwait	[expr {$max_file_upload_min * 60}] ;# 30, timeout for receive operations
	# ns_param	maxline		8192	;# 8192, max size of a header line
	# ns_param	maxheaders	128	;# 128, max number of header lines
	# ns_param	uploadpath	/tmp	;# directory for uploads
	# ns_param	backlog		256	;# 256, backlog for listen operations
	# ns_param	maxqueuesize	256	;# 1024, maximum size of the queue
	# ns_param	acceptsize	10	;# Maximum number of requests accepted at once.
	# ns_param	deferaccept     true    ;# false, Performance optimization, may cause recvwait to be ignored
	# ns_param	bufsize		16kB	;# 16kB, buffersize
	# ns_param	readahead	16kB	;# value of bufsize, size of readahead for requests
	# ns_param	sendwait	30	;# 30, timeout in seconds for send operations
	# ns_param	closewait	2	;# 2, timeout in seconds for close on socket
	# ns_param	keepwait	2	;# 5, timeout in seconds for keep-alive
	# ns_param	nodelay         false   ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
	# ns_param	keepalivemaxuploadsize	  500kB  ;# 0, don't allow keep-alive for upload content larger than this
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
	ns_param    extraheaders    $nssock_extraheaders
    }
    #
    # Define, which "host" (as supplied by the "host:" header
    # field) accepted over this driver should be associated with
    # which server.
    #
    ns_section ns/module/nssock/servers {
	ns_param $server $hostname
	ns_param $server $address
    }
}

#---------------------------------------------------------------------
# Configuration for HTTPS interface (SSL/TLS) -- module nsssl
#---------------------------------------------------------------------

if {[info exists httpsport]} {
    #
    # We have an "httpsport" configured, so configure this module.
    #
    ns_section ns/module/nsssl {
	ns_param defaultserver	$server
	ns_param address	$addresses
	ns_param port		$httpsport
	ns_param hostname	$hostname
	ns_param ciphers	"ECDH+AESGCM:DH+AESGCM:ECDH+AES256:DH+AES256:ECDH+AES128:DH+AES:ECDH+3DES:DH+3DES:RSA+AESGCM:RSA+AES:RSA+3DES:!aNULL:!MD5:!RC4"
	ns_param protocols	"!SSLv2:!SSLv3"
	ns_param certificate	$serverroot/etc/certfile.pem
	ns_param verify		0
	ns_param writerthreads	2
	ns_param writersize	1kB
	ns_param writerbufsize	16kB	;# 8kB, buffer size for writer threads
	#ns_param nodelay	false   ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
	#ns_param writerstreaming	true	;# false
	#ns_param deferaccept	true    ;# false, Performance optimization
	ns_param maxinput	${max_file_upload_mb}MB   ;# Maximum file size for uploads in bytes
	ns_param extraheaders	$nsssl_extraheaders
    }
    #
    # Define, which "host" (as supplied by the "host:" header
    # field) accepted over this driver should be associated with
    # which server.
    #
    ns_section ns/module/nsssl/servers {
	ns_param $server $hostname
	ns_param $server $address
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
    #ns_param	noextension	*/*
    #ns_param	.pcd		image/x-photo-cd
    #ns_param	.prc		application/x-pilot
}

#---------------------------------------------------------------------
# Global fastpath parameters
#---------------------------------------------------------------------
ns_section      "ns/fastpath" {
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
ns_section ns/server/${server} {
    #
    # Scaling and Tuning Options
    #
    # ns_param	maxconnections	100	;# 100; number of allocated connection structures
    # ns_param	maxthreads	10	;# 10; maximal number of connection threads
    ns_param	minthreads	2	;# 1; minimal number of connection threads

    ns_param	connsperthread	1000	;# 10000; number of connections (requests) handled per thread
    ;# Setting connsperthread to > 0 will cause the thread to
    ;# graciously exit, after processing that many
    ;# requests, thus initiating kind-of Tcl-level
    ;# garbage collection.

    # ns_param	threadtimeout	120	;# 120; timeout for idle threads.
    ;# In case, minthreads < maxthreads, threads
    ;# are shutdown after this idle time until
    ;# minthreads are reached.

    # ns_param	lowwatermark	10       ;# 10; create additional threads above this queue-full percentage
    ns_param	highwatermark	100      ;# 80; allow concurrent creates above this queue-is percentage
                                         ;# 100 means to disable concurrent creates
    #ns_param    connectionratelimit 200 ;# 0; limit rate per connection to this amount (KB/s); 0 means unlimited
    #ns_param    poolratelimit   200     ;# 0; limit rate for pool to this amount (KB/s); 0 means unlimited

    # Compress response character data: ns_return, ADP etc.
    #
    ns_param	compressenable	on	;# false, use "ns_conn compress" to override
    # ns_param	compresslevel	4	;# 4, 1-9 where 9 is high compression, high overhead
    # ns_param	compressminsize	512	;# Compress responses larger than this
    # ns_param	compresspreinit true	;# false, if true then initialize and allocate buffers at startup

    # Enable nicer directory listing (as handled by the OpenACS request processor)
    # ns_param	directorylisting	fancy	;# Can be simple or fancy

    #
    # Configuration of replies
    #
    # ns_param	realm		yourrealm	;# Default realm for Basic authentication
    # ns_param	noticedetail	false		;# true, return detail information in server reply
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
#       maxthreads
#       minthreads
#       poolratelimit
#       connectionratelimit
#       threadtimeout
#
# In order to define thread pools, do the following:
#
#  1. Add pool names to "ns/server/${server}/pools"
#  2. Configure pools with the noted parameters
#  3. Map method/URL combinations for these pools
#
#  All unmapped method/URL's will go to the default server pool of the
#  server.
#
########################################################################

ns_section "ns/server/${server}/pools" {
    #
    # To activate connection thread pools, uncomment one of the
    # following lines and/or add other pools.

    #ns_param   monitor	"Monitoring actions to check heathiness of the system"
    #ns_param   fast	"Fast requests, e.g. less than 10ms"
}

ns_section "ns/server/${server}/pool/monitor" {
    ns_param   minthreads 2
    ns_param   maxthreads 2

    ns_param   map "GET /admin/nsstats"
    ns_param   map "GET /SYSTEM"
    ns_param   map "GET /ds"
    ns_param   map "POST /ds"
    ns_param   map "GET /request-monitor"
}

ns_section "ns/server/${server}/pool/fast" {
    ns_param   minthreads 2
    ns_param   maxthreads 2

    ns_param   map "GET /*.png"
    ns_param   map "GET /*.PNG"
    ns_param   map "GET /*.jpg"
    ns_param   map "GET /*.pdf"
    ns_param   map "GET /*.gif"
    ns_param   map "GET /*.mp4"
    ns_param   map "GET /*.ts"
    ns_param   map "GET /*.m3u8"
}



#---------------------------------------------------------------------
# Special HTTP pages
#---------------------------------------------------------------------
ns_section ns/server/${server}/redirects {
    ns_param   404 /shared/404
    ns_param   403 /shared/403
    ns_param   503 /shared/503
    ns_param   500 /shared/500
}

#---------------------------------------------------------------------
# ADP (AOLserver Dynamic Page) configuration
#---------------------------------------------------------------------
ns_section ns/server/${server}/adp {
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

ns_section ns/server/${server}/adp/parsers {
    ns_param	fancy		".adp"
}

#
# Tcl Configuration
#
ns_section ns/server/${server}/tcl {
    ns_param	library		${serverroot}/tcl
    ns_param	debug		$debug
    # ns_param	nsvbuckets	16       ;# default: 8
}

ns_section "ns/server/${server}/fastpath" {
    ns_param	serverdir	${homedir}
    ns_param	pagedir		${pageroot}
    #
    # Directory listing options
    #
    # ns_param	directoryfile		"index.adp index.tcl index.html index.htm"
    # ns_param	directoryadp		$pageroot/dirlist.adp ;# Choose one or the other
    # ns_param	directoryproc		_ns_dirlist           ;#  ...but not both!
    # ns_param	directorylisting	fancy                 ;# Can be simple or fancy
    #
}
#---------------------------------------------------------------------
# OpenACS specific settings (per server)
#---------------------------------------------------------------------
#
# Define/override kernel parameters in section /acs
#
ns_section ns/server/${server}/acs {
    ns_param NsShutdownWithNonZeroExitCode 1
    # ns_param WithDeprecatedCode 0
    # ns_param LogIncludeUserId 1
    #
}

# Define/override OpenACS package parameters in section
# ending with /acs/PACKAGENAME
#
# Provide tailored sizes for the site node cache in acs-tcl:
#
ns_section ns/server/${server}/acs/acs-tcl {
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
ns_section ns/server/${server}/acs/acs-mail-lite {
    # ns_param EmailDeliveryMode log
}

#
# API browser configuration: setting IncludeCallingInfo to "true" is
# useful mostly for developer instances.
#
ns_section ns/server/${server}/acs/acs-api-browser {
    # ns_param IncludeCallingInfo true
}

#---------------------------------------------------------------------
# WebDAV Support (optional, requires oacs-dav package to be installed
#---------------------------------------------------------------------
ns_section ns/server/${server}/tdav {
    ns_param	propdir		   ${serverroot}/data/dav/properties
    ns_param	lockdir		   ${serverroot}/data/dav/locks
    ns_param	defaultlocktimeout 300
}

ns_section ns/server/${server}/tdav/shares {
    ns_param	share1		"OpenACS"
    # ns_param	share2		"Share 2 description"
}

ns_section ns/server/${server}/tdav/share/share1 {
    ns_param	uri		"/dav/*"
    # all WebDAV options
    ns_param	options		"OPTIONS COPY GET PUT MOVE DELETE HEAD MKCOL POST PROPFIND PROPPATCH LOCK UNLOCK"
}

#ns_section ns/server/${server}/tdav/share/share2 {
# ns_param	uri "/share2/path/*"
# read-only WebDAV options
# ns_param options "OPTIONS COPY GET HEAD MKCOL POST PROPFIND PROPPATCH"
#}


#---------------------------------------------------------------------
# Access log -- nslog
#---------------------------------------------------------------------
ns_section ns/server/${server}/module/nslog {
    #
    # General parameters for access.log
    #
    ns_param	file			${logroot}/access.log
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
    ns_param	checkforproxy	$proxy_mode ;# false, check for proxy header (X-Forwarded-For)
    ns_param	masklogaddr     true    ;# false, mask IP address in log file for GDPR (like anonip IP anonymizer)
    ns_param	maskipv4        255.255.255.0  ;# mask for IPv4 addresses
    ns_param	maskipv6        ff:ff:ff:ff::  ;# mask for IPv6 addresses

    #
    # Add extra entries to the access log via specifying a Tcl
    # list of request header fields in "extendedheaders"
    #
    if {[ns_config "ns/server/${server}/acs" LogIncludeUserId 0]} {
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
    ns_param	rollfmt		%Y-%m-%d ;# format appended to log file name
}

#---------------------------------------------------------------------
#
# CGI interface -- nscgi, if you have legacy stuff. Tcl or ADP files inside
# NaviServer are vastly superior to CGIs. I haven't tested these params but they
# should be right.
#
#---------------------------------------------------------------------
#ns_section "ns/server/${server}/module/nscgi"
#       ns_param	map	"GET  /cgi-bin ${serverroot}/cgi-bin"
#       ns_param	map	"POST /cgi-bin ${serverroot}/cgi-bin"
#       ns_param	Interps CGIinterps
#       ns_param        allowstaticresources true    ;# default: false

#ns_section "ns/interps/CGIinterps"
#       ns_param .pl "/usr/bin/perl"


#---------------------------------------------------------------------
#
# PAM authentication
#
#---------------------------------------------------------------------
ns_section ns/server/${server}/module/nspam {
    ns_param	PamDomain          "pam_domain"
}

#---------------------------------------------------------------------
#
# Database drivers
# The database driver is specified here.
# Make sure you have the driver compiled and put it in $bindir.
#
#---------------------------------------------------------------------
ns_section "ns/db/drivers" {

    if { $database eq "oracle" } {
	ns_param	ora8           ${bindir}/ora8
    } else {
	ns_param	postgres       ${bindir}/nsdbpg
	#
	ns_logctl severity "Debug(sql)" -color blue $verboseSQL
    }

    if { $database eq "oracle" } {
	ns_section "ns/db/driver/ora8"
	ns_param	maxStringLogLength -1
	ns_param	LobBufferSize      32768
    } else {
	ns_section "ns/db/driver/postgres"
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
ns_section ns/server/${server}/db {
    ns_param	pools              pool1,pool2,pool3
    ns_param	defaultpool        pool1
}
ns_section ns/db/pools {
    ns_param	pool1              "Pool 1"
    ns_param	pool2              "Pool 2"
    ns_param	pool3              "Pool 3"
}

ns_section ns/db/pool/pool1 {
    # ns_param	maxidle            0
    # ns_param	maxopen            0
    ns_param	connections        15
    ns_param    LogMinDuration     10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param	logsqlerrors       $debug
    if { $database eq "oracle" } {
	ns_param	driver             ora8
	ns_param	datasource         {}
	ns_param	user               $db_name
	ns_param	password           $db_password
    } else {
	ns_param	driver             postgres
	ns_param	datasource         ${db_host}:${db_port}:dbname=${db_name}
	ns_param	user               $db_user
	ns_param	password           ""
    }
}
#
# In case, you want to activate (more intense) SQL logging at runtime,
# consider the two commands (e.g. entered over ds/shell)
#
#    ns_logctl severity "Debug(sql)" on
#    ns_db logminduration pool1  10ms
#

ns_section ns/db/pool/pool2 {
    # ns_param	maxidle            0
    # ns_param	maxopen            0
    ns_param	connections        5
    ns_param    LogMinDuration     10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param	logsqlerrors       $debug
    if { $database eq "oracle" } {
	ns_param	driver             ora8
	ns_param	datasource         {}
	ns_param	user               $db_name
	ns_param	password           $db_password
    } else {
	ns_param	driver             postgres
	ns_param	datasource         ${db_host}:${db_port}:dbname=${db_name}
	ns_param	user               $db_user
	ns_param	password           ""
    }
}

ns_section ns/db/pool/pool3 {
    # ns_param	maxidle            0
    # ns_param	maxopen            0
    ns_param	connections        5
    # ns_param  LogMinDuration     0ms  ;# when SQL logging is on, log only statements above this duration
    ns_param	logsqlerrors       $debug
    if { $database eq "oracle" } {
	ns_param	driver             ora8
	ns_param	datasource         {}
	ns_param	user               $db_name
	ns_param	password           $db_password
    } else {
	ns_param	driver             postgres
	ns_param	datasource         ${db_host}:${db_port}:dbname=${db_name}
	ns_param	user               $db_user
	ns_param	password           ""
    }
}


#---------------------------------------------------------------------
# Which modules should be loaded for $server?  Missing modules break
# the server, so don't uncomment modules unless they have been
# installed.

ns_section ns/server/${server}/modules {
    ns_param	nslog		${bindir}/nslog
    ns_param	nsdb		${bindir}/nsdb
    ns_param	nsproxy		${bindir}/nsproxy

    #
    # Determine, if libthread is installed
    #
    set libthread [lindex [lsort [glob -nocomplain $homedir/lib/thread*/libthread*[info sharedlibextension]]] end]
    if {$libthread eq ""} {
	ns_log notice "No Tcl thread library installed in $homedir/lib/"
    } else {
	ns_param	libthread $libthread
	ns_log notice "Use Tcl thread library $libthread"
    }

    # authorize-gateway package requires dqd_utils
    # ns_param	dqd_utils dqd_utils[expr {int($tcl_version)}]

    # PAM authentication
    # ns_param	nspam              ${bindir}/nspam

    # LDAP authentication
    # ns_param	nsldap             ${bindir}/nsldap

    # These modules aren't used in standard OpenACS installs
    # ns_param	nsperm             ${bindir}/nsperm
    # ns_param	nscgi              ${bindir}/nscgi
}



#
# nsproxy configuration
#
ns_section ns/server/${server}/module/nsproxy {
    # ns_param	maxslaves          8
    # ns_param	sendtimeout        5000
    # ns_param	recvtimeout        5000
    # ns_param	waittimeout        100
    # ns_param	idletimeout        3000000
}

#
# nsstats configuration (global module)
#
# When installed under acs-subsite/www/admin/nsstats.tcl it is due to
# its /admin/ location save from public access.
#
ns_section "ns/module/nsstats" {
    ns_param enabled  1
    ns_param user     ""
    ns_param password ""
    ns_param bglocks  {oacs:sched_procs}
}

#
# If you want to activate core dumps, one can use the following command
#
#ns_log notice "nsd.tcl: ns_rlimit coresize [ns_rlimit coresize unlimited]"

ns_log notice "nsd.tcl: using threadsafe tcl: [info exists tcl_platform(threaded)]"
ns_log notice "nsd.tcl: finished reading config file."
