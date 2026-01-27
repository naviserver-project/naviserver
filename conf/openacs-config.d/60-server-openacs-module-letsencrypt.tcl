#---------------------------------------------------------------------
# Let's Encrypt -- extra module "letsencrypt"
#---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    #ns_param letsencrypt tcl
}
ns_section ns/server/$server/module/letsencrypt {

    # Provide one or more domain names (latter for multi-domain SAN
    # certificates). These values are a default in case the domains
    # are not provided by other means (e.g. "letsencrypt.tcl").  In
    # case multiple NaviServer virtual hosts are in used, this
    # definition must be on the $server, which is used for
    # obtaining updates (e.g. main site) although it retrieves a
    # certificate for many subsites.

    #ns_param domains { openacs.org openacs.net fisheye.openacs.org cvs.openacs.org }
}
