#---------------------------------------------------------------------
# Web Push for NaviServer -- extra module "nswebpush"
#---------------------------------------------------------------------
# To use this module:
#   1. Install the NaviServer module nswebpush
#   2. Add nswebpush to servermodules
#
if {"nswebpush" in $servermodules} {
    ns_section ns/server/$server/modules {
        ns_param nswebpush tcl
    }
}

