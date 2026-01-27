######################################################################
# Section 7 -- Final diagnostics / sample extras
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
