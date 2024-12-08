#
# Support for the TclPro debugger available from various sources
# such as
#     - https://github.com/flightaware/TclProDebug
#     - https://github.com/puremourning/TclProDebug
#     - https://github.com/apnadkarni/TclProDebug
#
# This hook procedure is called when NsAdpDebug() is invoked. An
# alternative hook can be defined via the adp configuration parameter
# "debuginit" in the configuration file.
#
# The hook procedure is based on the "remotedebug" documentation which
# is part of the TclPro debugger. It requires the installation of
# remotedebug/initdebug.tcl for the NaviServer instance.
#
# See the manual page of ns_adp_debug for details, how the TclPro
# debugger can be installed and used.
#

proc ns_adp_debuginit {procs host port} {
    #ns_log notice "+++ ns_adp_debuginit procs <$procs> host <$host> port <$port>"
    if {$host eq ""} {
        set host [ns_conn peeraddr]
    }
    if {![debugger_init $host $port]} {
        return -code error \
                "debugger_init: could not connect to $host:$port"
    }
    if {$procs ne ""} {
        try {
            #
            # Collect the script for instrumentalization using the
            # nstrace proc serializer.
            #
            set script [join [lmap p [info procs $procs] {
                if {[string match *debug* $p]} continue
                ::nstrace::_procscript $p
            }] \n]
            set fp [ns_opentmpfile procsfile]; puts $fp $script; close $fp
            #
            # The documentation of remote debugging the TclPro
            # Debugger recommends to use
            #
            #    debugger_eval { source ...}
            #
            # But this causes that one has first to skip over this
            # command in the Debugger GUI.  The following undocumented
            # trick (also used by AOLserver) avoids this.
            #
            DbgNub_sourceCmd $procsfile
            #debugger_eval { source $procsfile }
            
        } on error {errorMsg} {
            ns_log error "ns_adp_debuginit: $errorMsg"
        } finally {
            if {[info exists procsfile]} {
                file delete $procsfile
            }
        }
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
