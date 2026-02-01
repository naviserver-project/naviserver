########################################################################
#
# Section 2 -- Global network drivers & modules
#
########################################################################

# Name of the main server configuration
set server default

#
# Collect domain names and IP addresses associated with the main server
# configuration. Network drivers use this information to validate Host
# headers and map incoming requests to the correct server.
#
set server_set [ns_set create $server]
foreach domainname $hostname {
    ns_set put $server_set $server $domainname
}
foreach address $ipaddress {
    if {[ns_ip inany $address]} continue
    ns_set put $server_set $server $address
}

if {[namespace exists ::docker] && [info commands ::docker::map_to_server] eq ""} {
    #
    # Docker support: determine externally visible host:port mappings.
    #
    # When NaviServer runs inside a container, ports are often published on the
    # Docker host with a (potentially different) external address and port.
    # The returned mappings can be used to whitelist additional Host header
    # values (e.g., "host:port") for a given server configuration.
    #
    # Legacy implementation for setups where the docker environment does not
    # provide this helper.
    #
    proc ::docker::map_external_address_to_server {server port} {
        set label "$port/tcp"
        set s [ns_set create]
        if {$port eq ""
            || ![info exists ::docker::containerMapping]
            || ![dict exists $::docker::containerMapping $label]
        } {
            return $s
        }
        foreach {k info} $::docker::containerMapping {
            if {$k ne $label} continue
            set host    [dict get $info host]
            set pubport [dict get $info port]
            if {[ns_ip valid $host] && [ns_ip inany $host]} continue
            ns_set put $s $server ${host}:${pubport}
        }
        return $s
    }
}

#
# Shared network driver defaults (for HTTP and HTTPS)
#
ns_section ns/driver/common {
    ns_param defaultserver          $server
    ns_param address                $ipaddress
    ns_param hostname               [lindex $hostname 0]
    ns_param maxinput               $max_file_upload_size
    ns_param recvwait               $max_file_upload_duration
    ns_param keepalivemaxuploadsize 100kB
    ns_param writerthreads          1
    ns_param  writersize            1kB
}

#
# Global network modules (for all servers)
#
if {[info exists httpport] && $httpport ne ""} {
    #
    # We have an "httpport" configured, so configure this module.
    #
    ns_section ns/modules {
        ns_param http nssock
    }

    ns_section -update \
        -from ns/driver/common \
        ns/module/http {
        # ns_param defaultserver  $server
        # ns_param address       $ipaddress  ;# Space separated list of IP addresses
        # ns_param hostname      [lindex $hostname 0]
        ns_param port           $httpport

        # ns_param backlog       1024         ;# default: 256; backlog for listen operations
        # ns_param acceptsize    10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
        # ns_param sockacceptlog 3            ;# ns/param sockacceptlog; report, when one accept operation
                                             ;# receives more than this threshold number of sockets
        # ns_param closewait     0s           ;# default: 2s; timeout for close on socket
        # ns_param keepwait      5s           ;# 5s, timeout for keep-alive
        # ns_param maxqueuesize  1024         ;# default: 1024; maximum size of the queue

        # ns_param readahead     1MB          ;# default: 16384; size of readahead for requests
        # ns_param maxupload     1MB          ;# default: 0, when specified, spool uploads larger than this value to a temp file

        # ns_param maxinput       $max_file_upload_size      ;# Maximum file size for uploads
        # ns_param recvwait       $max_file_upload_duration  ;# 30s, timeout for receive operations
        # ns_param keepalivemaxuploadsize   0.5MB           ;# 0, don't allow keep-alive for upload content larger than this
        # ns_param keepalivemaxdownloadsize 1MB             ;# 0, don't allow keep-alive for download content larger than this

        #
        # Spooling Threads
        #
        # ns_param  writerthreads   1           ;# default: 0, number of writer threads
        # ns_param writersize      1KB         ;# default: 1MB, use writer threads for files larger than this value
        # ns_param writerbufsize   16kB        ;# default: 8kB, buffer size for writer threads
        # ns_param writerstreaming true        ;# false;  activate writer for streaming HTML output (e.g. ns_writer)

        # ns_param spoolerthreads  1           ;# default: 0; number of upload spooler threads
        # ns_param driverthreads   2           ;# default: 1, number of driver threads (requires support of SO_REUSEPORT)

        #
        # TCP tuning
        #
        # ns_param nodelay         false       ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        # ns_param deferaccept     true        ;# false, Performance optimization

        #
        # Extra response header fields for this driver
        #
        ns_param extraheaders    $http_extraheaders
    }

    #
    # Register known Host header values for this driver and map them to server
    # configurations.
    #
    ns_section -set $server_set ns/module/http/servers {}
    if {[namespace exists ::docker]} {
        ns_section -set [::docker::map_external_address_to_server $server $httpport] ns/module/http/servers {}
    }
}

if {[info exists httpsport] && $httpsport ne ""} {
    #
    # We have an "httpsport" configured, so load and configure the
    # module "nsssl" as a global server module with the name "https".
    #
    ns_section ns/modules {
        ns_param https nsssl
    }

    ns_section -update \
        -from ns/driver/common \
        ns/module/https {
        # ns_param defaultserver $server
        # ns_param address       $ipaddress
        # ns_param hostname      [lindex $hostname 0]
        ns_param port            $httpsport

        # ns_param backlog       1024         ;# default: 256; backlog for listen operations
        # ns_param acceptsize    10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
        # ns_param sockacceptlog 3            ;# ns/param sockacceptlog; report, when one accept operation
                                              ;# receives more than this threshold number of sockets
        # ns_param closewait     0s           ;# default: 2s; timeout for close on socket
        # ns_param keepwait      5s           ;# 5s, timeout for keep-alive
        # ns_param maxqueuesize  1024         ;# default: 1024; maximum size of the queue

        # ns_param readahead     1MB          ;# default: 16384; size of readahead for requests
        # ns_param maxupload     1MB          ;# default: 0, when specified, spool uploads larger than this value to a temp file

        # ns_param maxinput      $max_file_upload_size      ;# Maximum file size for uploads
        # ns_param recvwait      $max_file_upload_duration  ;# 30s, timeout for receive operations
        # ns_param keepalivemaxuploadsize   0.5MB           ;# 0, don't allow keep-alive for upload content larger than this
        # ns_param keepalivemaxdownloadsize 1MB             ;# 0, don't allow keep-alive for download content larger than this

        #
        # Spooling Threads
        #
        # ns_param  writerthreads   1          ;# default: 0, number of writer threads
        # ns_param writersize      1KB         ;# default: 1MB, use writer threads for files larger than this value
        # ns_param writerbufsize   16kB        ;# default: 8kB, buffer size for writer threads
        # ns_param writerstreaming true        ;# false;  activate writer for streaming HTML output (e.g. ns_writer)

        # ns_param spoolerthreads  1           ;# default: 0; number of upload spooler threads
        # ns_param driverthreads   2           ;# default: 1, number of driver threads (requires support of SO_REUSEPORT)

        #
        # TCP tuning
        #
        # ns_param nodelay         false       ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        # ns_param deferaccept     true        ;# false, Performance optimization

        #
        # SSL/TLS parameters
        #
        ns_param ciphers         "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"
        #ns_param ciphersuites   "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"
        ns_param protocols       "!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"
        ns_param certificate     $certificate
        #ns_param vhostcertificates $home/etc/certificates ;# directory for vhost certificates of the default server
        ns_param verify          0

        # OCSP stapling configuration:
        # ns_param OCSPstapling    on          ;# off; activate OCSP stapling
        # ns_param OCSPstaplingVerbose  on    ;# off; make OCSP stapling more verbose
        # ns_param OCSPcheckInterval 15m      ;# default 5m; OCSP (re)check intervale

        ns_param extraheaders    $https_extraheaders
    }

    #
    # Define, which "host" (as supplied by the "host:" header field)
    # accepted over this driver should be associated with which server.
    #
    ns_section -set $server_set ns/module/https/servers {}
    if {[namespace exists ::docker]} {
        ns_section -set [::docker::map_external_address_to_server $server $httpsport] ns/module/https/servers {}
    }
}

#
# (Kept here as a minimal virtual-host sample; note that the earlier
#  ns/module/http/servers block already registers $hostname and $ipaddress.)
#
ns_section ns/module/http/servers {
    ns_param $server    localhost
    ns_param $server    [ns_info hostname]
}
