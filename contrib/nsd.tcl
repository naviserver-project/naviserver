set             home            	/usr/local/ns
set		server			default

ns_section      "ns/server/${server}/modules"
ns_param        nscp            	${home}/bin/nscp.so
ns_param        nssock          	${home}/bin/nssock.so
ns_param        nslog           	${home}/bin/nslog.so 
ns_param        nscgi			${home}/bin/nscgi.so
ns_param        nsdb            	${home}/bin/nsdb.so

ns_section	"ns/parameters"
ns_param	home			$home
ns_param	user			nobody
ns_param	group			nobody
ns_param	debug			true
ns_param	logroll			true
ns_param	tcllibrary		${home}/tcl
ns_param	serverlog		${home}/logs/nsd.log
ns_param	pidfile			${home}/logs/nsd.pid
ns_param        outputcharset   	iso8859-1
ns_param        urlcharset      	iso8859-1

ns_section	"ns/threads"
ns_param	stacksize		[expr 1024*1024]

ns_section	"ns/mimetypes"
ns_param	default         	text/plain
ns_param	noextension     	text/plain
source					${home}/conf/mimetypes.tcl

ns_section	"ns/db/drivers"
#ns_param        postgres        	${home}/bin/nspostgres.so

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
ns_param	${server}		"Naviserver"

ns_section	"ns/server/${server}"
ns_param	pageroot		${home}/pages
ns_param	globalstats     	true
ns_param	urlstats        	true
ns_param	maxurlstats     	1000
ns_param        checkmodifiedsince      true
ns_param	enabletclpages  	true
ns_param	maxthreads		100
ns_param	maxconnections		100
ns_param	threadtimeout		1800

ns_section	"ns/server/${server}/db"
ns_param	pools			*

ns_section	"ns/server/${server}/fastpath"
ns_param	serverdir		${home}
ns_param	pagedir        		pages
ns_param	directoryfile		"index.adp index.tcl index.html index.htm"
ns_param	directoryproc   	_ns_dirlist           
ns_param	directorylisting 	fancy

ns_section	"ns/server/${server}/vhost"
ns_param	enabled			false
ns_param	hostprefix		""
ns_param	hosthashlevel		0
ns_param	stripport		true
ns_param	stripwww		true

ns_section	"ns/server/${server}/adp"
ns_param	map             	"/*.adp"
ns_param	fancy			true
ns_param	defaultparser		fancy
ns_param	enableexpire    	false
ns_param	enabledebug     	true
ns_param	cache			true
ns_param	cachesize		[expr 10000*1024]
ns_param	taglocks		false

ns_section	"ns/server/${server}/adp/parsers"
ns_param	fancy			".adp"

ns_section	"ns/server/${server}/tcl"
ns_param	debug			false
ns_param	nsvbuckets		16
ns_param	library			${home}

ns_section      "ns/server/${server}/module/nscgi"
ns_param        map                     "GET  /cgi-bin ${home}/cgi-bin"
ns_param        map                     "POST /cgi-bin ${home}/cgi-bin"
ns_param        interps                 interps

ns_section	"ns/server/${server}/module/nslog"
ns_param	file			${home}/logs/access.log
ns_param	rolllog         	true
ns_param	rollonsignal    	false
ns_param	rollhour        	0
ns_param	maxbackup       	7

ns_section      "ns/server/${server}/module/nssock"
ns_param        port                    8080
ns_param	address			0.0.0.0
ns_param        hostname                [ns_info hostname]
ns_param	maxinput		[expr 1024*1024*10]
ns_param	readahead		[expr 1024*1024*1]
ns_param	spoolerthreads		1
ns_param	uploadsize		[expr 1024*1024*1]
ns_param	writerthreads		0
ns_param	writersize		[expr 1024*1024*5]

ns_section      "ns/server/${server}/module/nscp"
ns_param        port            	4080
ns_param        address         	127.0.0.1

ns_section	"ns/server/${server}/module/nscp/users"
ns_param        user            	"::"

ns_section 	ns/server/stats
ns_param 	enabled 		1
ns_param 	url 			/_stats
ns_param 	user 			nsadmin
ns_param 	password 		nsadmin
