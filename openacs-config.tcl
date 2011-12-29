###################################################################### 
#
# Config parameter for an OpenACS site using naviserver.
#
# These default settings will only work in limited circumstances.
# Two servers with default settings cannot run on the same host
#
###################################################################### 
ns_log notice "nsd.tcl: starting to read config file..."

#---------------------------------------------------------------------
# change to 80 and 443 for production use
set httpport                  8000
set httpsport                 8443 
# If setting port below 1024 with NaviServer, read comments in file:
#  /usr/local/ns/service0/packages/etc/daemontools/run

# The hostname and address should be set to actual values.
# setting the address to 0.0.0.0 means aolserver listens on all interfaces
set hostname                  localhost
set address                   127.0.0.1

# Note: If port is privileged (usually < 1024), OpenACS must be
# started by root, and, in NaviServer, the run script have a 
# '-b address' flag which matches the address according to settings (above)

set server                    "service0" 
set servername                "New OpenACS Installation - Development"

set serverroot                "/var/www/${server}"

#---------------------------------------------------------------------
# which database do you want? postgres or oracle
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
# if debug is false, all debugging will be turned off
set debug false
set dev   false

set homedir                   /usr/local/ns
set bindir                    ${homedir}/bin

set max_file_upload_mb        20
set max_file_upload_min        5

###################################################################### 
#
# End of instance-specific settings 
#
# Nothing below this point need be changed in a default install.
#
###################################################################### 


#---------------------------------------------------------------------
#
# NaviServer's directories. Autoconfigurable. 
#
#---------------------------------------------------------------------
# Where are your pages going to live ?
set pageroot                  ${serverroot}/www 
set directoryfile             index.tcl,index.adp,index.html,index.htm


#---------------------------------------------------------------------
# Global server parameters 
#---------------------------------------------------------------------
ns_section ns/parameters 
    ns_param   serverlog	${serverroot}/log/error.log 
    ns_param   pidfile		${serverroot}/log/nsd.pid
    ns_param   home		$homedir 
    ns_param   debug		$debug
    #
    #ns_param   logroll		on
    #ns_param   logmaxbackup	10
    #ns_param   maxbackup	100
    ns_param   logdebug		$debug
    ns_param   logdev		$dev

    #ns_param   mailhost	localhost 
    #ns_param   jobsperthread	0
    #ns_param   schedsperthread	0

    #
    # Encoding settings (see http://dqd.com/~mayoff/encoding-doc.html)
    #
    #ns_param   HackContentType	1
    #     
    # Defaults charsets are utf-8
    #ns_param   OutputCharset	utf-8
    #ns_param   URLCharset	utf-8

#---------------------------------------------------------------------
# Thread library (nsthread) parameters 
#---------------------------------------------------------------------
ns_section ns/threads 
    # The per-thread stack size must be a multiple of 8k for NaviServer to run under MacOS X
    ns_param   stacksize          [expr {128 * 8192}]

# 
# MIME types. 
# 
ns_section ns/mimetypes
    #  Note: NaviServer already has an exhaustive list of MIME types:
    #  see: /usr/local/src/naviserver/nsd/mimetypes.c
    #  but in case something is missing you can add it here. 
    ns_param   Default            */*
    ns_param   NoExtension        */*
    ns_param   .pcd               image/x-photo-cd
    ns_param   .prc               application/x-pilot
    ns_param   .xls               application/vnd.ms-excel
    ns_param   .doc               application/vnd.ms-word


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
ns_section ns/servers 
	ns_param $server		$servername 

# 
# Server parameters 
# 
ns_section ns/server/${server} 
	ns_param   directoryfile	$directoryfile
	ns_param   pageroot		$pageroot
	#
	# Scaling and Tuning Options
	#
	ns_param   keepwait		0    ;# 30, keepalive period
	#ns_param   maxconnections	100  ;# number of allocated connection stuctures
	#ns_param   maxthreads		10   ;# max number of connection threads
	#ns_param   minthreads          0    ;# min number of connection threads
	#ns_param   connsperthread	0    ;# number of connections handled per thread
	#ns_param   threadtimeout	120  ;# timeout for idle theads
	#
	# Directory listing options
	#
	#ns_param   directoryadp	$pageroot/dirlist.adp ;# Choose one or the other
	#ns_param   directoryproc	_ns_dirlist           ;#  ...but not both!
	#ns_param   directorylisting	fancy                 ;# Can be simple or fancy

#
# Special HTTP pages
#
ns_section ns/server/${server}/redirects
    ns_param   404 "/global/file-not-found.html"
    ns_param   403 "/global/forbidden.html"
    ns_param   503 "/global/busy.html"
    ns_param   500 "/global/error.html"

#---------------------------------------------------------------------
# 
# ADP (AOLserver Dynamic Page) configuration 
# 
#---------------------------------------------------------------------
ns_section ns/server/${server}/adp 
	ns_param   map                /*.adp    ;# Extensions to parse as ADP's 
	#ns_param   map			"/*.html" ;# Any extension can be mapped 
	#ns_param   enableexpire	false     ;# Set "Expires: now" on all ADP's 
	ns_param   enabledebug		$debug    ;# Allow Tclpro debugging with "?debug"

ns_section ns/server/${server}/adp/parsers
	ns_param   fancy ".adp"

# 
# Tcl Configuration 
# 
ns_section ns/server/${server}/tcl
    ns_param   library            ${serverroot}/tcl
    ns_param   autoclose          on 
    ns_param   debug              $debug
 
ns_section "ns/server/${server}/fastpath"
	ns_param        serverdir             ${homedir}
	ns_param        pagedir               ${pageroot}

#---------------------------------------------------------------------
#
# WebDAV Support (optional, requires oacs-dav package to be installed
#
#---------------------------------------------------------------------
ns_section ns/server/${server}/tdav
    ns_param propdir ${serverroot}/data/dav/properties
    ns_param lockdir ${serverroot}/data/dav/locks
    ns_param defaultlocktimeout "300"

ns_section ns/server/${server}/tdav/shares
    ns_param share1 "OpenACS"
#    ns_param share2 "Share 2 description"

ns_section ns/server/${server}/tdav/share/share1
    ns_param uri "/dav/*"
    # all WebDAV options
    ns_param options "OPTIONS COPY GET PUT MOVE DELETE HEAD MKCOL POST PROPFIND PROPPATCH LOCK UNLOCK"

#ns_section ns/server/${server}/tdav/share/share2
#    ns_param uri "/share2/path/*"
    # read-only WebDAV options
#    ns_param options "OPTIONS COPY GET HEAD MKCOL POST PROPFIND PROPPATCH"


#---------------------------------------------------------------------
# 
# Socket driver module (HTTP)  -- nssock 
# 
#---------------------------------------------------------------------
ns_section ns/server/${server}/module/nssock
    ns_param   timeout            120
    ns_param   address            $address
    ns_param   hostname           $hostname
    ns_param   port               $httpport
# setting maxinput higher than practical may leave the server vulnerable to resource DoS attacks
# see http://www.panoptic.com/wiki/aolserver/166
    ns_param   maxinput           [expr {$max_file_upload_mb * 1024 * 1024}] ;# Maximum File Size for uploads in bytes
    ns_param   maxpost            [expr {$max_file_upload_mb * 1024 * 1024}] ;# Maximum File Size for uploads in bytes
    ns_param   recvwait           [expr {$max_file_upload_min * 60}] ;# Maximum request time in minutes

# maxsock will limit the number of simultanously returned pages,
# regardless of what maxthreads is saying
    ns_param   maxsock               100 ;# 100 = default

# On Windows you need to set this parameter to define the number of
# connections as well (it seems).
    ns_param   backlog               5  ;# if < 1 == 5 

# Optional params with defaults:
    ns_param   acceptsize            10; #1
    ns_param   bufsize               16000
    ns_param   rcvbuf                0
    ns_param   sndbuf                0
    ns_param   socktimeout           30 ;# if < 1 == 30
    ns_param   sendwait              30 ;# if < 1 == socktimeout
    ns_param   recvwait              30 ;# if < 1 == socktimeout
    ns_param   closewait             2  ;# if < 0 == 2
    ns_param   keepwait              30 ;# if < 0 == 30
    ns_param   readtimeoutlogging    false
    ns_param   serverrejectlogging   false
    ns_param   sockerrorlogging      false
    ns_param   sockshuterrorlogging  false

#---------------------------------------------------------------------
# 
# Access log -- nslog 
# 
#---------------------------------------------------------------------
ns_section ns/server/${server}/module/nslog 
    ns_param   debug              false
    ns_param   dev                false
    ns_param   enablehostnamelookup false
    ns_param   file               ${serverroot}/log/${server}.log
    ns_param   logcombined        true
    ns_param   extendedheaders    COOKIE
#    ns_param   logrefer           false
#    ns_param   loguseragent       false
    ns_param   logreqtime         true
    ns_param   maxbackup          1000
    ns_param   rollday            *
    ns_param   rollfmt            %Y-%m-%d-%H:%M
    ns_param   rollhour           0
    ns_param   rollonsignal       true
    ns_param   rolllog            true

#---------------------------------------------------------------------
#
# nsjava - aolserver module that embeds a java virtual machine.  Needed to 
#          support webmail.  See http://nsjava.sourceforge.net for further 
#          details. This may need to be updated for OpenACS4 webmail
#
#---------------------------------------------------------------------
ns_section ns/server/${server}/module/nsjava
    ns_param   enablejava         off  ;# Set to on to enable nsjava.
    ns_param   verbosejvm         off  ;# Same as command line -debug.
    ns_param   loglevel           Notice
    ns_param   destroyjvm         off  ;# Destroy jvm on shutdown.
    ns_param   disablejitcompiler off  
    ns_param   classpath          /usr/local/jdk/jdk118_v1/lib/classes.zip:${bindir}/nsjava.jar:${pageroot}/webmail/java/activation.jar:${pageroot}/webmail/java/mail.jar:${pageroot}/webmail/java 

#---------------------------------------------------------------------
# 
# CGI interface -- nscgi, if you have legacy stuff. Tcl or ADP files inside 
# NaviServer are vastly superior to CGIs. I haven't tested these params but they
# should be right.
# 
#---------------------------------------------------------------------
#ns_section "ns/server/${server}/module/nscgi" 
#       ns_param   map "GET  /cgi-bin ${serverroot}/cgi-bin"
#       ns_param   map "POST /cgi-bin ${serverroot}/cgi-bin" 
#       ns_param   Interps CGIinterps

#ns_section "ns/interps/CGIinterps" 
#       ns_param .pl "/usr/bin/perl"


#---------------------------------------------------------------------
#
# PAM authentication
#
#---------------------------------------------------------------------
ns_section ns/server/${server}/module/nspam
    ns_param   PamDomain          "pam_domain"


#---------------------------------------------------------------------
#
# OpenSSL for Aolserver  4
# 
#---------------------------------------------------------------------

ns_section "ns/server/${server}/module/nsopenssl"

    # this is used by acs-tcl/tcl/security-procs.tcl to get the https port.
    ns_param ServerPort                $httpsport
    # setting maxinput higher than practical may leave the server vulnerable to resource DoS attacks
    # see http://www.panoptic.com/wiki/aolserver/166
    # must set maxinput for nsopenssl as well as nssock
    ns_param   maxinput           [expr {$max_file_upload_mb * 1024 * 1024}] ;# Maximum File Size for uploads in bytes

    # We explicitly tell the server which SSL contexts to use as defaults when an
    # SSL context is not specified for a particular client or server SSL
    # connection. Driver connections do not use defaults; they must be explicitly
    # specificied in the driver section. The Tcl API will use the defaults as there
    # is currently no provision to specify which SSL context to use for a
    # particular connection via an ns_openssl Tcl command.
ns_section "ns/server/${server}/module/nsopenssl/sslcontexts"
    ns_param users        "SSL context used for regular user access"
    #    ns_param admins       "SSL context used for administrator access"
    ns_param client       "SSL context used for outgoing script socket connections"

ns_section "ns/server/${server}/module/nsopenssl/defaults"
    ns_param server               users
    ns_param client               client
    
ns_section "ns/server/${server}/module/nsopenssl/sslcontext/users"
    ns_param Role                  server
    ns_param ModuleDir             ${serverroot}/etc/certs
    ns_param CertFile              users-certfile.pem 
    ns_param KeyFile               users-keyfile.pem
    # CADir/CAFile can be commented out, if CA chain cert is appended to CA issued server cert.
    ns_param CADir                 ${serverroot}/etc/certs
    ns_param CAFile                users-ca.crt
    # for Protocols                "ALL" = "SSLv2, SSLv3, TLSv1"
    ns_param Protocols             "SSLv3, TLSv1" 
    ns_param CipherSuite           "ALL:!ADH:RC4+RSA:+HIGH:+MEDIUM:+LOW:+SSLv2:+EXP" 
    ns_param PeerVerify            false
    ns_param PeerVerifyDepth       3
    ns_param Trace                 false
    
    # following helps to stablize some openssl connections from buggy clients.
    ns_param SessionCache true
    ns_param SessionCacheID 1
    ns_param SessionCacheSize 512
    ns_param SessionCacheTimeout 300


#    ns_section "ns/server/${server}/module/nsopenssl/sslcontext/admins"
#    ns_param Role                  server
#    ns_param ModuleDir             /path/to/dir
#    ns_param CertFile              server/server.crt 
#    ns_param KeyFile               server/server.key 
#    ns_param CADir                 ca-client/dir 
#    ns_param CAFile                ca-client/ca-client.crt
    # for Protocols                "ALL" = "SSLv2, SSLv3, TLSv1"
#    ns_param Protocols             "All"
#    ns_param CipherSuite           "ALL:!ADH:RC4+RSA:+HIGH:+MEDIUM:+LOW:+SSLv2:+EXP" 
#    ns_param PeerVerify            false
#    ns_param PeerVerifyDepth       3
#    ns_param Trace                 false
    
ns_section "ns/server/${server}/module/nsopenssl/sslcontext/client"
    ns_param Role                  client
    ns_param ModuleDir             ${serverroot}/etc/certs
    ns_param CertFile              client-certfile.pem
    ns_param KeyFile               client-keyfile.pem 
    # CADir/CAFile can be commented out, if CA chain cert is appended to CA issued server cert.
    ns_param CADir                 ${serverroot}/etc/certs
    ns_param CAFile                client-ca.crt
    # for Protocols                "ALL" = "SSLv2, SSLv3, TLSv1"
    ns_param Protocols             "SSLv2, SSLv3, TLSv1" 
    ns_param CipherSuite           "ALL:!ADH:RC4+RSA:+HIGH:+MEDIUM:+LOW:+SSLv2:+EXP" 
    ns_param PeerVerify            false
    ns_param PeerVerifyDepth       3
    ns_param Trace                 false

# following helps to stablize some openssl connections to buggy servers.
    ns_param SessionCache true
    ns_param SessionCacheID 1
    ns_param SessionCacheSize 512
    ns_param SessionCacheTimeout 300

# SSL drivers. Each driver defines a port to listen on and an explitictly named
# SSL context to associate with it. Note that you can now have multiple driver
# connections within a single virtual server, which can be tied to different
# SSL contexts.
ns_section "ns/server/${server}/module/nsopenssl/ssldrivers"
    ns_param users         "Driver for regular user access"
#    ns_param admins        "Driver for administrator access"

ns_section "ns/server/${server}/module/nsopenssl/ssldriver/users"
    ns_param sslcontext            users
    # ns_param port                  $httpsport_users
    ns_param port                  $httpsport
    ns_param hostname              $hostname
    ns_param address               $address
    # following added per
    # http://www.mail-archive.com/aolserver@listserv.aol.com/msg07365.html
    # Maximum File Size for uploads:
    ns_param   maxinput           [expr {$max_file_upload_mb * 1024 * 1024}] ;# in bytes
    # Maximum request time
    ns_param   recvwait           [expr {$max_file_upload_min * 60}] ;# in minutes

#    ns_section "ns/server/${server}/module/nsopenssl/ssldriver/admins"
#    ns_param sslcontext            admins
#    ns_param port                  $httpsport_admins
#    ns_param port                  $httpsport
#    ns_param hostname              $hostname
#    ns_param address               $address


#---------------------------------------------------------------------
# 
# Database drivers 
# The database driver is specified here.
# Make sure you have the driver compiled and put it in {aolserverdir}/bin
#
#---------------------------------------------------------------------
ns_section "ns/db/drivers" 
    if { $database eq "oracle" } {
        ns_param   ora8           ${bindir}/ora8.so
    } else {
        ns_param   postgres       ${bindir}/nsdbpg.so  ;# Load PostgreSQL driver
    }

    if { $database eq "oracle" } {
        ns_section "ns/db/driver/ora8"
        ns_param  maxStringLogLength -1
        ns_param  LobBufferSize      32768
    }

 
# Database Pools: This is how NaviServer  ``talks'' to the RDBMS. You need 
# three for OpenACS: main, log, subquery. Make sure to replace ``yourdb'' 
# and ``yourpassword'' with the actual values for your db name and the 
# password for it, if needed.  
#
# NaviServer can have different pools connecting to different databases 
# and even different different database servers.  See
# http://openacs.org/doc/tutorial-second-database.html
# An example 'other db' configuration is included (and commented out) using other1_db_name
# set other1_db_name "yourDBname"

ns_section ns/db/pools 
    ns_param   pool1              "Pool 1"
    ns_param   pool2              "Pool 2"
    ns_param   pool3              "Pool 3"
#    ns_param   pool4              "Pool4 Other1"
#    ns_param   pool5              "Pool5 Other1"
#    ns_param   pool6              "Pool6 Other1"

ns_section ns/db/pool/pool1
    ns_param   maxidle            0
    ns_param   maxopen            0
    ns_param   connections        15
    ns_param   verbose            $debug
    ns_param   extendedtableinfo  true
    ns_param   logsqlerrors       $debug
    if { $database eq "oracle" } {
        ns_param   driver             ora8
        ns_param   datasource         {}
        ns_param   user               $db_name
        ns_param   password           $db_password
    } else {
        ns_param   driver             postgres 
        ns_param   datasource         ${db_host}:${db_port}:${db_name}
        ns_param   user               $db_user
        ns_param   password           ""
    } 

ns_section ns/db/pool/pool2
    ns_param   maxidle            0
    ns_param   maxopen            0
    ns_param   connections        5
    ns_param   verbose            $debug
    ns_param   extendedtableinfo  true
    ns_param   logsqlerrors       $debug
    if { $database eq "oracle" } {
        ns_param   driver             ora8
        ns_param   datasource         {}
        ns_param   user               $db_name
        ns_param   password           $db_password
    } else {
        ns_param   driver             postgres 
        ns_param   datasource         ${db_host}:${db_port}:${db_name}
        ns_param   user               $db_user
        ns_param   password           ""
    } 

ns_section ns/db/pool/pool3
    ns_param   maxidle            0
    ns_param   maxopen            0
    ns_param   connections        5
    ns_param   verbose            $debug
    ns_param   extendedtableinfo  true
    ns_param   logsqlerrors       $debug
    if { $database eq "oracle" } {
        ns_param   driver             ora8
        ns_param   datasource         {}
        ns_param   user               $db_name
        ns_param   password           $db_password
    } else {
        ns_param   driver             postgres 
        ns_param   datasource         ${db_host}:${db_port}:${db_name}
        ns_param   user               $db_user
        ns_param   password           ""
    } 

# ns_section ns/db/pool/pool4
#    ns_param   maxidle            0
#    ns_param   maxopen            0
#    ns_param   connections        5
#    ns_param   verbose            $debug
#    ns_param   extendedtableinfo  true
#    ns_param   logsqlerrors       $debug
#    if { $database eq "oracle" } {
#        ns_param   driver             ora8
#        ns_param   datasource         {}
#        ns_param   user               $db_name
#        ns_param   password           $db_password
#    } else {
#        ns_param   driver             postgres 
#        ns_param   datasource         ${db_host}:${db_port}:${other1_db_name}
#        ns_param   user               $db_user
#        ns_param   password           ""
#    } 

# ns_section ns/db/pool/pool5
# ...
# ns_section ns/db/pool/pool6
# ...


ns_section ns/server/${server}/db
    ns_param   pools              pool1,pool2,pool3
# if a second db is added, add the pools here. for example, replace above line with:
#    ns_param   pools              pool1,pool2,pool3,pool4,pool5,pool6
    ns_param   defaultpool        pool1

# following from http://openacs.org/doc/tutorial-second-database.html
#ns_section ns/server/${server}/acs/database
#    ns_param database_names [list main other1]
#    ns_param pools_main [list pool1 pool2 pool3]
#    ns_param pools_other1 [list pool4 pool5 pool6]
# Start each pool set with pools_* 
# The code assumes the name in database_names matches the suffix to pools_ in one of the ns_params.



#---------------------------------------------------------------------
# which modules should be loaded?  Missing modules break the server, so
# don't uncomment modules unless they have been installed.
ns_section ns/server/${server}/modules 
    ns_param   nssock             ${bindir}/nssock.so 
    ns_param   nslog              ${bindir}/nslog.so 
    ns_param   nsdb               ${bindir}/nsdb.so
    ns_param   libthread          [lindex [glob ${homedir}/lib/thread*/libthread*[info sharedlibextension]] 0]

    # openacs versions earlier than 5.x requires nsxml
#    ns_param nsxml              ${bindir}/nsxml.so

    #---------------------------------------------------------------------
    # nsopenssl will fail unless the cert files are present as specified
    # later in this file, so it's disabled by default
#    ns_param   nsopenssl          ${bindir}/nsopenssl.so

    # authorize-gateway package requires dqd_utils
    # ns_param   dqd_utils dqd_utils[expr {int($tcl_version)}].so

    # Full Text Search
#    ns_param   nsfts              ${bindir}/nsfts.so

    # PAM authentication
#    ns_param   nspam              ${bindir}/nspam.so

    # LDAP authentication
#    ns_param   nsldap             ${bindir}/nsldap.so

    # These modules aren't used in standard OpenACS installs
#    ns_param   nsperm             ${bindir}/nsperm.so 
#    ns_param   nscgi              ${bindir}/nscgi.so 
#    ns_param   nsjava             ${bindir}/libnsjava.so
#    ns_param   nsrewrite          ${bindir}/nsrewrite.so 


#
# nsproxy configuration
#

ns_section ns/server/${server}/module/nsproxy
#     ns_param   maxslaves          8
#     ns_param   sendtimeout        5000
#     ns_param   recvtimeout        5000
#     ns_param   waittimeout        1000
#     ns_param   idletimeout        300000

ns_log notice "nsd.tcl: using threadsafe tcl: [info exists tcl_platform(threaded)]"
ns_log notice "nsd.tcl: finished reading config file."
