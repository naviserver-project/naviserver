#---------------------------------------------------------------------
# Interactive Shell for NaviServer -- extra module "nsshell"
#---------------------------------------------------------------------
# To use this module:
#   1. Install the NaviServer module nsshell
#   2. Add 'nsshell' to 'servermodules'
#   3. Configure nsshell parameters as needed
#
if {"nsshell" in $servermodules} {
    ns_section ns/server/$server/modules {
        ns_param    nsshell   tcl
    }
    ns_section ns/server/$server/module/nsshell {
        ns_param    url                 /nsshell
        ns_param    kernel_heartbeat    5
        ns_param    kernel_timeout      10
    }
}
