######################################################################
# Section 5 -- Global utility modules
######################################################################

#---------------------------------------------------------------------
# Statistics Module -- extra module "nsstats"
#
# When installed under acs-subsite/www/admin/nsstats.tcl it is, due to
# its /admin/ location, safe from public access.
#
# This section only configures the module; loading is optional and
# typically controlled via ns/modules (see comment below).
#---------------------------------------------------------------------
if {"nsstats" in $extramodules} {
    ns_section ns/module/nsstats {
        ns_param enabled  1
        ns_param user     ""
        ns_param password ""
        ns_param bglocks  {oacs:sched_procs}
    }

    # The nsstats module consists of a single file, there is no need to
    # load it as a (Tcl) module, once the file is copied.
}



