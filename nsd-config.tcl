set             home            	/usr/local/ns

ns_section      "ns/server/default/modules"
#ns_param        nscp            	nscp.so
ns_param        nssock          	nssock.so
ns_param        nslog           	nslog.so
ns_param        nscgi			nscgi.so
ns_param        nsdb            	nsdb.so

ns_section	"ns/parameters"
ns_param	home			$home
ns_param	debug			true
ns_param	logroll			true
ns_param	tcllibrary		${home}/tcl
ns_param	serverlog		${home}/logs/nsd.log
ns_param	pidfile			${home}/logs/nsd.pid
ns_param        outputcharset   	iso8859-1
ns_param        urlcharset      	iso8859-1
ns_param	dbcloseonexit		off
ns_param	jobsperthread		1000
ns_param	jobtimeout		0

ns_param        smtphost                "localhost"
ns_param        smtpport                25
ns_param        smtptimeout             60
ns_param        smtplogmode             false
ns_param        smtpmsgid               false
ns_param        smtpmsgidhostname       ""
ns_param        smtpencodingmode        false
ns_param        smtpencoding            "utf-8"
ns_param        smtpauthmode            ""
ns_param        smtpauthuser            ""
ns_param        smtpauthpassword        ""

ns_section	"ns/threads"
ns_param	stacksize		[expr 512*1024]

ns_section	"ns/mimetypes"
ns_param	default         	text/plain
ns_param	noextension     	text/plain

ns_section	"ns/db/drivers"
#ns_param        postgres        	nsdbpg.so

ns_section	"ns/db/pools"
#ns_param	postgres		"PostgresSQL Database"

ns_section      "ns/db/pool/pgsql"
ns_param        driver          	postgres
ns_param        connections     	64
ns_param	user			postgres
ns_param        datasource      	"::dbname"
ns_param	verbose			Off
ns_param	logsqlerrors		On
ns_param	extendedtableinfo	On
ns_param        maxidle                 31536000
ns_param        maxopen                 31536000

ns_section	"ns/servers"
ns_param	default		"Naviserver"

ns_section	"ns/server/default"
ns_param	globalstats     	true
ns_param	urlstats        	true
ns_param	maxurlstats     	1000
ns_param        checkmodifiedsince      true
ns_param	enabletclpages  	true
ns_param	connsperthread		1000
ns_param        minthreads              5
ns_param	maxthreads		100
ns_param	maxconnections		100
ns_param	threadtimeout		1800

ns_section	"ns/server/default/db"
ns_param	pools			*

ns_section	"ns/server/default/fastpath"
ns_param	serverdir		${home}
ns_param	pagedir        		pages
ns_param	directoryfile		"index.adp index.tcl index.html index.htm"
ns_param	directoryproc   	_ns_dirlist
ns_param	directorylisting 	fancy

ns_section	"ns/server/default/vhost"
ns_param	enabled			false
ns_param	hostprefix		""
ns_param	hosthashlevel		0
ns_param	stripport		true
ns_param	stripwww		true

ns_section	"ns/server/default/adp"
ns_param	map             	"/*.adp"
ns_param	enableexpire    	false
ns_param	enabledebug     	true
ns_param	singlescript		false
ns_param	nocache			false
ns_param	cache			true
ns_param	cachesize		[expr 5000*1024]

ns_section	"ns/server/default/adp/parsers"
ns_param	fancy			".adp"

ns_section	"ns/server/default/tcl"
ns_param	nsvbuckets		16
ns_param	library			${home}/modules/tcl

ns_section      "ns/server/default/module/nscgi"
ns_param        map                     "GET  /cgi-bin ${home}/cgi-bin"
ns_param        map                     "POST /cgi-bin ${home}/cgi-bin"
ns_param        interps                 interps

ns_section	"ns/server/default/module/nslog"
ns_param	file			${home}/logs/access.log
ns_param	rolllog         	true
ns_param	rollonsignal    	false
ns_param	rollhour        	0
ns_param	maxbackup       	7

ns_section      "ns/server/default/module/nssock"
ns_param        port                    8080
ns_param	address			0.0.0.0
ns_param        hostname                [ns_info hostname]
ns_param	maxinput		[expr 1024*1024*10]
ns_param	readahead		[expr 1024*1024*1]
ns_param	spoolerthreads		1
ns_param	uploadsize		[expr 1024*1024*1]
ns_param	writerthreads		0
ns_param	writersize		[expr 1024*1024*5]
ns_param        backlog                 1024
ns_param        acceptsize              10
ns_param        closewait               0
ns_param        rcvbuf                  512000
ns_param        sndbuf                  512000
ns_param        maxqueuesize            1024

ns_section      "ns/server/default/module/nscp"
ns_param        port            	4080
ns_param        address         	127.0.0.1

ns_section	"ns/server/default/module/nscp/users"
ns_param        user            	"::"
