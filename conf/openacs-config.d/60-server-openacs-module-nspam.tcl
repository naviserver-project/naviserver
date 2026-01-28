#---------------------------------------------------------------------
# PAM authentication -- extra module "nspam"
#---------------------------------------------------------------------
# To use this module:
#   1. Install the NaviServer module nspam
#   2. Add 'nspam' to 'servermodules'
#   3. Configure nspam parameters as needed
#
if {"nspam" in $servermodules} {
    ns_section ns/server/$server/modules {
        ns_param nspam nspam
    }
    ns_section ns/server/$server/module/nspam {
        ns_param PamDomain "pam_domain"
    }
}

