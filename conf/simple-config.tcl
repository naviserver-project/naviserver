#
# This is a fairly minimal NaviServer configuration file that makes the server to
# accept HTTP requests on 0.0.0.0:8000 (IPv4)
#
# Logs are in the logs/nsd.log and logs/access.log
#
# When the nscp module is enabled it will accept telnet into nscp on
# [::1]:2080 (IPv6) or 127.0.0.1:2080 (IPv4)
#

ns_section ns/servers {
    ns_param    default         NaviServer
}
ns_section ns/server/default/adp {
    ns_param    map              /*.adp
}
if {1} {
ns_section ns/server/default/modules {
    #ns_param   nscp            nscp
    ns_param    nssock          nssock
    ns_param    nslog           nslog
}
ns_section ns/server/default/module/nssock {
    ns_param    address         0.0.0.0
    ns_param    port            8000
}
} else {
ns_section ns/modules { ns_param    nssock          nssock }
ns_section ns/module/nssock {
    ns_param    address         0.0.0.0
    ns_param    port            8000
}
}
#ns_logctl severity Debug(ns:driver) on
