#
# NaviServer configuration file for testing paths.
#
# This configuration file can be called with "params" setting to
# influence the configurable parameters like the following
#
#    params="serverdir /opt/local/var/www logdir log" /usr/local/ns/bin/nsd -f -t test-paths-config.tcl
#
# or the configurable parameters can be set here in this file.
#
# Parameters:
#
#   - bindir
#   - homedir
#   - libdir
#   - logdir
#   - serverdir
#   - serverinitfile
#   - serverlibdir
#   - serverlogdir
#   - serverpagedir
#   - serverrootproc
#

if {[info exists ::env(params)]} {
    set params [join $::env(params) " "]
    puts stderr "======================================================= USE PARAMS: $params "
    dict with params {}
} else {
    #set serverdir /var/www3
    #set serverpagedir www
    #set serverrootproc {ns_log notice SERVERROOTPROC; return /SERVERDIR}
    set serverdir s0
    set serverlogdir log

    set params {}
    foreach p {logdir serverdir serverpagedir serverlogdir serverrootproc} {
        if {[info exists $p]} {dict set params $p [set $p]}
    }
}

set address 0.0.0.0
set httpport 8080

# -------------------------------------------------------------------------
# Global parameters
# -------------------------------------------------------------------------
ns_section ns/parameters {
    #ns_param serverlog		/dev/stdout
    ns_param listenurl http://localhost:$httpport
    ns_param params $params
    if {[info exists homedir]} {ns_param home $homedir}
    if {[info exists logdir]}  {ns_param logdir $logdir}
    if {[info exists libdir]}  {ns_param tcllibrary $libdir}
    if {[info exists bindir]}  {
        puts stderr "SETTING BINDIR TO <$bindir>"
        ns_param bindir $bindir
    }
}
ns_section ns/servers {
    ns_param SERVER1 WebServer
}

ns_section ns/modules {
    ns_param http nssock.so
}
ns_section ns/module/http {
    ns_param address            $address
    ns_param port               $httpport
}

# -------------------------------------------------------------------------
# Server specific setup
# -------------------------------------------------------------------------
ns_section ns/server/SERVER1 {
    ns_param enabletclpages      true
    if {[info exists serverlogdir]} {ns_param logdir $serverlogdir}
    if {[info exists serverrootproc]} {ns_param serverrootproc $serverrootproc}
}

ns_section ns/server/SERVER1/modules {
    ns_param nslog nslog.so
}
ns_section ns/server/SERVER1/module/nslog {
    ns_param rollonsignal true
}
ns_section ns/server/SERVER1/fastpath {
    if {[info exists serverdir]}     {ns_param serverdir $serverdir}
    if {[info exists serverpagedir]} {ns_param pagedir $serverpagedir}
}

ns_section ns/server/SERVER1/tcl {
    if {[info exists serverinitfile]} {ns_param initfile $serverinitfile}
    if {[info exists serverlibdir]} {ns_param library $serverlibdir}
    ns_param enabletclpages true
    # if {1 && [info exists serverrootproc]} {
    #     set initcmds [subst {
    #         ns_serverrootproc {$serverrootproc}
    #     }]
    # }
    append initcmds {
        ns_register_proc GET /info {
            set f %-25s
            ns_return 200 text/plain [subst {
                [format $f params] [list [ns_config ns/parameters params]]
                [format $f homedir] [ns_info home]
                [format $f serverdir] [ns_server serverdir]
                [format $f "serverdir-effective"] [ns_server serverdir -effective]
                [format $f serverlogdir] [ns_server logdir]
                [format $f serverpagedir] [ns_server pagedir]
                [format $f url2file] [ns_url2file /]
            }]
        }
        ns_log notice === INIT [ns_info server]
        ns_atstartup {
            ns_log notice === START [ns_info server]
            ns_log notice [ns_http run [ns_config ns/parameters listenurl]/info]
            ns_shutdown
        }
    }
    #ns_log notice === CONFIG $initcmds
    ns_param initcmds $initcmds
}
