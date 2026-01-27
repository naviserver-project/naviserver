########################################################################
#
# Section 0 -- Bootstrap & defaults (pure Tcl)
#
########################################################################

if {[info commands ::ns_configure_variables] eq ""} {
    ns_log notice "backward compatibility hook (pre NaviServer 5): have to source init.tcl"
    source [file normalize [file dirname [file dirname [ns_info nsd]]]/tcl/init.tcl]
}

# All default variables in "defaultConfig" can be overloaded by:
#
# 1) Setting these variables explicitly in this file after
#    "ns_configure_variables" (highest precedence)
#
# 2) Setting these variables as environment variables with the "nsd_"
#    prefix (suitable for e.g. docker setups).  The lookup for
#    environment variables happens in "ns_configure_variables".
#
# 3) Alter/override the variables in the "defaultConfig"
#    (lowest precedence)
#
# Some comments:
#   "ipaddress":
#       specify an IPv4 or IPv6 address, or a blank separated
#       list of such addresses
#   "httpport":
#       might be as well a list of ports, when listening on
#       multiple ports
#   "nscpport":
#       when nonempty, load the nscp module and listen
#       on the specified port
#   "home":
#       the root directory, containng the subdirecturies
#       "pages", "logs", "lib", "bin", "tcl", ....
#
dict set defaultConfig ipaddress   0.0.0.0
dict set defaultConfig httpport    8080
dict set defaultConfig httpsport   ""
dict set defaultConfig nscpport    ""
dict set defaultConfig home        [file dirname [file dirname [info nameofexecutable]]]
dict set defaultConfig hostname    localhost
dict set defaultConfig pagedir     {$home/pages}
dict set defaultConfig logdir      {$home/logs}
dict set defaultConfig certificate {$home/etc/server.pem}
dict set defaultConfig vhostcertificates {$home/etc/certificates}
dict set defaultConfig serverprettyname "My NaviServer Instance"
dict set defaultConfig reverseproxymode false
dict set defaultConfig trustedservers ""
dict set defaultConfig enablehttpproxy false
dict set defaultConfig setupfile ""
dict set defaultConfig max_file_upload_size  20MB
dict set defaultConfig max_file_upload_duration 5m

#
# For all potential variables defined by the dict "defaultConfig",
# allow environment variables with the prefix "nsd_" (such as
# "nsd_httpport" or "nsd_ipaddress") to override local values.
#
ns_configure_variables "nsd_" $defaultConfig

#---------------------------------------------------------------------
# Set headers that should be included in every response from the
# server.
#
set http_extraheaders {
    x-frame-options            "SAMEORIGIN"
    x-content-type-options     "nosniff"
    x-xss-protection           "1; mode=block"
    referrer-policy            "strict-origin"
}

set https_extraheaders {
    strict-transport-security "max-age=63072000; includeSubDomains"
}
append https_extraheaders $http_extraheaders

#
# Environment defaults used by OpenSSL and some tools.
# Keep these in bootstrap, since later fragments may rely on them.
#
set ::env(RANDFILE) $home/.rnd
set ::env(HOME) $home
set ::env(LANG) en_US.UTF-8
