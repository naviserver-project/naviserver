#
# This is minimal Naviserver config file that makes the server to
# accept HTTP requests on port 80
#
# Logs are in the logs/nsd.log and logs/access.log
#
# Once nscp module is enabled it will accept telnet into nscp on 127.0.0.1:2080
#

ns_section      "ns/servers"
ns_param         default         Naviserver

ns_section      "ns/server/default/modules"
ns_param         nscp            nscp.so
ns_param         nssock          nssock.so
ns_param         nslog           nslog.so

