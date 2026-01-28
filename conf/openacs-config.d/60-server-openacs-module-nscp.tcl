#---------------------------------------------------------------------
# NaviServer Control Port -- core module "nscp"
# ---------------------------------------------------------------------
# This module lets you connect to a specified host and port using a
# telnet client to administer the server and execute database commands
# on the running system.
#
# Details about enabling and configuration:
#     https://naviserver.sourceforge.io/n/nscp/files/nscp.html
#
# To use this module:
#   1. Configure 'nscpport' to a non-empty value

if {$nscpport ne ""} {
    ns_section ns/server/$server/modules {
        ns_param nscp nscp
    }
    ns_section ns/server/$server/module/nscp {
        ns_param port $nscpport
        ns_param address  127.0.0.1        ;# default: 127.0.0.1 or ::1 for IPv6
        #ns_param echopasswd on            ;# default: off
        ns_param cpcmdlogging on           ;# default: off
        #ns_param allowLoopbackEmptyUser on ;# default: off
    }
    ns_section ns/server/$server/module/nscp/users {
        ns_param user "nsadmin:t2GqvvaiIUbF2:"
    }
}
