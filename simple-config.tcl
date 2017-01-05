#
# This is minimal NaviServer config file that makes the server to
# accept HTTP requests on [::]:80 (IPv6) or on 0.0.0.0:80 (IPv4)
#
# Logs are in the logs/nsd.log and logs/access.log
#
# Once nscp module is enabled it will accept telnet into nscp on
# [::1]:2080 (IPv6) or 127.0.0.1:2080 (IPv4)
#

ns_section      "ns/servers"
ns_param         default         NaviServer

ns_section      "ns/server/default/modules"
ns_param         nscp            nscp.so
ns_param         nssock          nssock.so
ns_param         nslog           nslog.so

ns_section     "ns/server/default/adp"
ns_param        map              /*.adp
