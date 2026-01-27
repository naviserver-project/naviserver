
######################################################################
#
# Generic OpenACS Site Configuration for NaviServer
#
# Either provide configuration values from the command line (see
# below) or provide different values directly in the configuration
# file.
#
######################################################################

######################################################################
#
# Configuration structure overview
#
# Section 0 -- Bootstrap & defaults (pure Tcl)
#    - Logging and environment checks
#    - defaultConfig dictionary and overrides (ns_configure_variables, CLI)
#    - Derivation of basic variables: server, serverroot, homedir, logdir,
#      hostname/ipaddress/httpport/httpsport, pageroot, etc.
#
# Section 1 -- Global NaviServer parameters (ns/parameters)
#    - Core ns/parameters
#    - Optional reverse proxy mode (ns/parameters/reverseproxymode)
#
# Section 2 -- Global network drivers & modules
#    - HTTP driver (nssock) as module "http"
#    - HTTPS driver (nsssl) as module "https"
#    - Global driver options and virtual-host mappings
#
# Section 3 -- Global runtime configuration
#    - Thread library parameters (ns/threads)
#    - Extra MIME types (ns/mimetypes)
#    - Global fastpath configuration (ns/fastpath)
#
# Section 4 -- Global database drivers and pools
#    - ns/db/drivers
#    - ns/db/driver-specific settings (e.g. postgres, nsoracle)
#    - ns/db/pools and pool definitions
#
# Section 5 -- Global utility modules
#    - Global modules not bound to a specific server
#      (e.g. nsstats, future monitoring or helper modules)
#
# Section 6 -- Server configurations
#    - Pools, redirects, ADP, Tcl, fastpath, HTTP client,
#      per-server modules;
#
# Section 7 -- Final diagnostics / sample extras
#    - Final ns_log diagnostics
#
######################################################################

######################################################################
# Section 0 -- Bootstrap & defaults (pure Tcl)
######################################################################
ns_log notice "nsd.tcl: starting to read configuration file..."

# Define default configuration values in a dictionary.  These can be
# overridden from ns_configure_variables, command-line options, or by
# redefining the Tcl variables later in this file.
#

set defaultConfig {
    hostname          localhost
    ipaddress         127.0.0.1
    httpport          8000
    httpsport         ""
    nscpport          ""
    smtpdport         ""
    smtprelay         $hostname:25

    server            "openacs"
    serverprettyname  "My OpenACS Instance"
    serverroot        /var/www/$server
    homedir           "[file dirname [file dirname [ns_info nsd]]]"
    logdir            $serverroot/log
    certificate       $serverroot/etc/certfile.pem
    vhostcertificates $serverroot/etc/certificates

    dbms              postgres
    db_host           localhost
    db_port           ""
    db_name           $server
    db_user           nsadmin
    db_password       ""
    db_passwordfile   ""

    CookieNamespace   ad_

    max_file_upload_size      20MB
    max_file_upload_duration   5m

    reverseproxymode  false
    trustedservers    ""
    cachingmode       full

    clusterSecret     ""
    parameterSecret   ""

    debug             false
    verboseSQL        false

    setupfile         ""
    extramodules      ""
}


# Optionally override the default configuration variables defined in
# "defaultConfig" dictionary via "dict set" commands (this allows you
# to comment out lines as needed).
#
# Example: When the same domain name is used for multiple OpenACS
# instances, using the same cookie namespace for these instances can
# cause conflicts. Consider setting a unique namespace for cookies.
#
#    dict set defaultConfig CookieNamespace ad_8000_
#
#
# For Oracle, we provide different default values for convenience
#
if { [dict get $defaultConfig dbms] eq "oracle" } {
    dict set defaultConfig db_password "openacs"
    dict set defaultConfig db_name openacs
    dict set defaultConfig db_port 1521
    #
    # Traditionally, the old configs have all db_user set to the
    # db_name, e.g. "openacs".
    dict set defaultConfig db_user [dict get defaultConfig server]

    set ::env(ORACLE_HOME) /opt/oracle/product/19c/dbhome_1
    set ::env(NLS_DATE_FORMAT) YYYY-MM-DD
    set ::env(NLS_TIMESTAMP_FORMAT) "YYYY-MM-DD HH24:MI:SS.FF6"
    set ::env(NLS_TIMESTAMP_TZ_FORMAT) "YYYY-MM-DD HH24:MI:SS.FF6 TZH:TZM"
    set ::env(NLS_LANG) American_America.UTF8
}

#
# Now turn the keys from the default value dictionary into local Tcl
# variables. In this step every default value will be overwritten by a
# shell variables with the prefix "oacs_" if provided. To override
# e.g. the HTTP port, call nsd like in the following example:
#
#    oacs_httpport=8100 ... /usr/local/ns/bin/nsd ...
#
# Optional per-instance setup file:
#
# If the dict contains the key "setupfile" and it is non-empty,
# ns_configure_variables will source this Tcl file *after* applying
# environment variable overrides.  The setup file is intended to hold
# instance-specific variable assignments (no ns_section blocks), e.g. for
# multiple developer instances sharing the same installation tree.
#
# When "setupfile" is a relative path, it is resolved relative to the
# configuration root derived from [ns_info config] (the -t argument):
#   - if -t is a directory:  <configdir>/<setupfile>
#   - if -t is a file:       <dirname(-t)>/<setupfile>
#

# Check for the existence of the command "ns_configure_variables".
# For backward compatibility with pre-NaviServer 5, source init.tcl if not found.
if {[info commands ::ns_configure_variables] eq ""} {
    ns_log notice "backward compatibility hook (pre NaviServer 5): have to source init.tcl"
    source [file normalize [file dirname [file dirname [ns_info nsd]]]/tcl/init.tcl]
}

ns_configure_variables "oacs_" $defaultConfig

#
# One can set here more variables (or hard-coded overwrite the values
# from the defaultConfig dictionary) here. These are standard values,
# where we assume, this are on every OpenACSS instance the same.
#
set pageroot                  ${serverroot}/www
set directoryfile             "index.tcl index.adp index.html index.htm"

#
# In case we have a db_passwordfile, use the content of the file as
# the database password. Can be used, e.g., for docker secrets.
#
if {$db_passwordfile ne "" && [file readable $db_passwordfile]} {
    try {
        set F [open $db_passwordfile]
        set db_password [string trim [read $F]]
    } finally {
        close $F
        unset F
    }
}

#
# For Oracle, we set the datasource to values which might be
# changed via environment variables. So, this has to happen
# after "ns_configure_variables"
#
if { $dbms eq "oracle" } {
    set datasource ${db_host}:${db_port}/$db_name ;# name of the pluggable database / service
} else {
    set datasource ${db_host}:${db_port}:dbname=${db_name}
}

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

#---------------------------------------------------------------------
# Set environment variables HOME and LANG. HOME is needed since
# otherwise some programs called via exec might try to write into the
# root home directory.
#
set ::env(HOME) $homedir
set ::env(LANG) en_US.UTF-8

#ns_logctl severity "Debug(ns:driver)" $debug

