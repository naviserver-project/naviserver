#---------------------------------------------------------------------
# OpenACS-specific server general configuration
#---------------------------------------------------------------------
# Define/override OpenACS kernel parameter for $server
#
ns_section ns/server/$server/acs {
    #------------------------------------------------------------------
    # Cookie namespace and static CSP rules
    #------------------------------------------------------------------
    # Optionally use a different cookie namespace (used as a prefix for
    # OpenACS cookies). This is important when, for example, multiple
    # servers run on different ports of the same host but must not share
    # login/session cookies.
    #
    ns_param CookieNamespace $CookieNamespace

    # Mapping between MIME types and CSP rules for static files.
    # The value is a Tcl dict, used e.g. by
    #   security::csp::add_static_resource_header
    #
    # Example below disables script execution from inline SVG images.
    #
    ns_param StaticCSP {
        image/svg+xml "script-src 'none'"
    }

    #------------------------------------------------------------------
    # Host header validation (OpenACS level)
    #------------------------------------------------------------------
    # Whitelist for Host header values, as seen by OpenACS.
    #
    # The configuration file may contain a list of hostnames accepted
    # as values of the Host header field (typically domain name with
    # optional port). Validation is needed, for example, to avoid
    # accepting spoofed host headers that could hijack redirects to a
    # different site. This is usually necessary when running behind a
    # proxy or in containerized setups where the Host header does not
    # directly match any driver configuration.
    #
    ns_param whitelistedHosts {}

    #------------------------------------------------------------------
    # Deprecated code loading (compatibility)
    #------------------------------------------------------------------
    # The parameter "WithDeprecatedCode" controls whether OpenACS
    # core/library files should load deprecated compatibility shims.
    # Set this to 1 for legacy sites that still rely on old APIs.
    #
    # Note: Setting this parameter to 0 may break packages that still
    # depend on deprecated interfaces.
    #
    # ns_param WithDeprecatedCode true    ;# default: false

    #------------------------------------------------------------------
    # Server restart behaviour (platform-specific)
    #------------------------------------------------------------------
    # When set to 1, acs-admin/server-restart uses "ns_shutdown -restart"
    # instead of plain "ns_shutdown". This is required on some Windows
    # installations. Default is 0.
    #
    # ns_param NsShutdownWithNonZeroExitCode 1

    #------------------------------------------------------------------
    # Logging and privacy
    #------------------------------------------------------------------
    # Include user_ids in log files? Some sensitive sites forbid this.
    # Default is 0 (do not include user_ids in logs).
    #
    # ns_param LogIncludeUserId 1

    #------------------------------------------------------------------
    # Cluster and security secrets
    #------------------------------------------------------------------
    # Cluster secret for intra-cluster communication. Clustering will
    # not be enabled if no value is provided.
    #
    ns_param clusterSecret   $clusterSecret

    # Secret used for signing query and form parameters (e.g. security
    # tokens in URLs and forms).
    #
    ns_param parameterSecret             $parameterSecret
}

#---------------------------------------------------------------------
# OpenACS-specific server per package configuration
#---------------------------------------------------------------------
# Define/override OpenACS package parameters in section
# ending with /acs/PACKAGENAME
#
ns_section ns/server/$server/acs/acs-tcl {
    # Example cache sizes; adjust for your installation:
    #
    # ns_param SiteNodesCacheSize        2000000
    # ns_param SiteNodesIdCacheSize       100000
    # ns_param SiteNodesChildenCacheSize  100000
    # ns_param SiteNodesPrefetch  {/file /changelogs /munin}
    # ns_param UserInfoCacheSize          2000000
}


ns_section ns/server/$server/acs/acs-mail-lite {
    # Setting EmailDeliveryMode to "log" is useful for developer
    # instances. Typically set in OpenACS package parameters, be we
    # can override here.
    # ns_param EmailDeliveryMode log      ;# or "nssmtpd" when using the nssmtpd module
}

ns_section ns/server/$server/acs/acs-api-browser {
    # ns_param IncludeCallingInfo true    ;# useful mostly on development instances
}
