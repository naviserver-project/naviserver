
#
# Set the IP-address and port, on which the server listens. Since we
# want this script to run in IPv4 and IPv6 environments (and in IPv6
# environments, where IPv6 is deactivated) independent of the OS, we
# probe the interfaces here before we set the final IP address.
#
set port 8080
set loopback "127.0.0.1"

if {[ns_info ipv6]} {
    #
    # The version of NaviServer supports IPv6. Probe if we can revese
    # lookup the loopback interface.
    #
    if {![catch {ns_hostbyaddr ::1}]} {
	#
	# Yes we can. So use the IPv6 style loopback address
	#
	set loopback ::1
    }
}
set address $loopback

#set             home                /usr/local/ns
set             home                [file dirname [file dirname [info nameofexecutable]]]

ns_section     "ns/server/default/modules"
ns_param        nscp                nscp.so
ns_param        nssock              nssock.so
ns_param        nslog               nslog.so
ns_param        nscgi               nscgi.so
ns_param        nsdb                nsdb.so

ns_section     "ns/parameters"
ns_param        home                $home
ns_param        tcllibrary          tcl
#ns_param        tclinitlock         true	       ;# default: false
ns_param        serverlog           error.log
ns_param        pidfile             nsd.pid
#ns_param       logdebug            true               ;# default: false
#ns_param       logroll             false              ;# default: true
#ns_param       dbcloseonexit       off                ;# default: off; from nsdb
ns_param        jobsperthread       1000               ;# default: 0
ns_param        jobtimeout          0                  ;# default: 300
ns_param        schedsperthread     10                 ;# default: 0
ns_param        progressminsize     [expr 1024*1024*1] ;# default: 0

# configure SMTP module
ns_param        smtphost            "localhost"
ns_param        smtpport            25
ns_param        smtptimeout         60
ns_param        smtplogmode         false
ns_param        smtpmsgid           false
ns_param        smtpmsgidhostname   ""
ns_param        smtpencodingmode    false
ns_param        smtpencoding        "utf-8"
ns_param        smtpauthmode        ""
ns_param        smtpauthuser        ""
ns_param        smtpauthpassword    ""

ns_section     "ns/threads"
ns_param        stacksize           [expr {512*1024}]

ns_section     "ns/mimetypes"
ns_param        default             text/plain
ns_param        noextension         text/plain

ns_section     "ns/db/drivers"
#ns_param       postgres           nsdbpg.so

ns_section     "ns/db/pools"
#ns_param       postgres           "PostgresSQL Database"

ns_section     "ns/db/pool/pgsql"
ns_param        driver              postgres
ns_param        connections         64
ns_param        user                postgres
ns_param        datasource          "::dbname"
ns_param        verbose             Off
ns_param        logsqlerrors        On
ns_param        extendedtableinfo   On
ns_param        maxidle             31536000
ns_param        maxopen             31536000

ns_section     "ns/servers"
ns_param        default             "Naviserver"

ns_section     "ns/server/default"
ns_param        checkmodifiedsince  false ;# default: true, check modified-since before returning files from cache. Disable for speedup
ns_param        connsperthread      1000  ;# default: 0; number of connections (requests) handled per thread
ns_param        minthreads          5     ;# default: 1; minimal number of connection threads
ns_param        maxthreads          100   ;# default: 10; maximal number of connection threads
ns_param        maxconnections      100   ;# default: 100; number of allocated connection stuctures
ns_param        threadtimeout       120   ;# default: 120; timeout for idle theads
#ns_param concurrentcreatethreshold 100   ;# default: 80; allow concrruent creates when queue is fully beyond this percentage
                                          ;# 100 is a concervative value, disabling concurrent creates

ns_section     "ns/server/default/db"
ns_param        pools               *

ns_section      "ns/fastpath"
ns_param        cache               false     ;# default: false
ns_param        cachemaxsize        10240000  ;# default: 1024*10000
ns_param        cachemaxentry       8192      ;# default: 8192
ns_param        mmap                false     ;# default: false

ns_section     "ns/server/default/fastpath"
ns_param        pagedir             pages
#ns_param       serverdir           ""
ns_param        directoryfile       "index.adp index.tcl index.html index.htm"
ns_param        directoryproc       _ns_dirlist
ns_param        directorylisting    fancy    ;# default: simple
#ns_param        directoryadp       dir.adp

ns_section     "ns/server/default/vhost"
ns_param        enabled             false
ns_param        hostprefix          ""
ns_param        hosthashlevel       0
ns_param        stripport           true
ns_param        stripwww            true

ns_section     "ns/server/default/adp"
ns_param        map                 "/*.adp"
ns_param        enableexpire        false    ;# default: false; set "Expires: now" on all ADP's 
#ns_param        enabledebug         true     ;# default: false
ns_param        enabletclpages      true     ;# default: false
ns_param        singlescript        false    ;# default: false; collapse Tcl blocks to a single Tcl script
ns_param        cache               false    ;# default: false; enable ADP caching
ns_param        cachesize           [expr 5000*1024]

ns_section     "ns/server/default/tcl"
ns_param        nsvbuckets          16       ;# default: 8
ns_param        library             modules/tcl

ns_section     "ns/server/default/module/nscgi"
ns_param        map                 "GET  /cgi-bin $home/cgi-bin"
ns_param        map                 "POST /cgi-bin $home/cgi-bin"
ns_param        interps             CGIinterps

ns_section "ns/interps/CGIinterps"
ns_param	.pl		    "/opt/local/bin/perl"
ns_param	.sh		    "/bin/bash"

ns_section     "ns/server/default/module/nslog"
ns_param        file                access.log
ns_param        rolllog             true     ;# default: true; should server log files automatically
ns_param        rollonsignal        false    ;# default: false; perform roll on a sighup
ns_param        rollhour            0        ;# default: 0; specify at which hour to roll
ns_param        maxbackup           7        ;# default: 10; max number of backup log files
#ns_param       rollfmt		    %Y-%m-%d-%H:%M	;# format appendend to log file name

ns_section     "ns/server/default/module/nssock"
ns_param        port                 $port
#ns_param        address             0.0.0.0
#ns_param        address             ::0   ;# ::1 corresponds to 127.0.0.1, ::0 is the "unspecified address"
ns_param        address             $address
ns_param        hostname            [ns_info hostname]
ns_param        maxinput            [expr 1024*1024*10] ;# default: 1024*1024, maximum size for inputs (uploads)
ns_param        readahead           [expr 1024*1024*1]  ;# default: 16384; size of readahead for requests
ns_param        backlog             1024         ;# default: 256; backlog for listen operations
ns_param        acceptsize          10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
ns_param        closewait           0            ;# default: 2; timeout in seconds for close on socket
ns_param        maxqueuesize        1024         ;# default: 1024; maximum size of the queue
ns_param        keepwait	    5	         ;# 5, timeout in seconds for keep-alive
ns_param        keepalivemaxuploadsize	 500000	 ;# 0, don't allow keep-alive for upload content larger than this
ns_param        keepalivemaxdownloadsize 1000000 ;# 0, don't allow keep-alive for download content larger than this
#
# Spooling Threads
#
#ns_param       spoolerthreads	1	;# default: 0; number of upload spooler threads
#ns_param       maxupload	0       ;# default: 0, when specified, spool uploads larger than this value to a temp file
#ns_param       writerthreads	1	;# default: 0, number of writer threads
#ns_param       writersize	1048576	;# default: 1024*1024, use writer threads for files larger than this value
#ns_param       writerbufsize	8192	;# default: 8192, buffer size for writer threads


ns_section     "ns/server/default/module/nscp"
ns_param        port                4080
#ns_param        address             127.0.0.1
ns_param        address             $address

ns_section     "ns/server/default/module/nscp/users"
ns_param        user                "::"
