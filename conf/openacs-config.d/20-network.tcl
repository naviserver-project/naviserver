######################################################################
# Section 2 -- Global network drivers (HTTP/HTTPS)
######################################################################

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
    ns_param writerthreads          2
    ns_param  writersize            1kB
}

# Configuration for plain HTTP interface -- core module "nssock"
#---------------------------------------------------------------------
if {[info exists httpport] && $httpport ne ""} {
    #
    # We have an "httpport" configured, so load and configure the
    # module "nssock" as a global server module with the name "http".
    #
    ns_section ns/modules {
        ns_param http nssock
    }

    ns_section -update \
        -from ns/driver/common \
        ns/module/http {
        #------------------------------------------------------------------
        # Basic binding and request size limits
        #------------------------------------------------------------------
        # ns_param defaultserver $server
        # ns_param address       $ipaddress
        # ns_param hostname      [lindex $hostname 0]
        ns_param port          $httpport              ;# default: 80

        # Per-driver limits for incoming requests; override ns/parameters
        # maxinput/recvwait if those are set there.
        # ns_param maxinput      $max_file_upload_size  ;# maximum size for request bodies (e.g. uploads)
        # ns_param recvwait      $max_file_upload_duration ;# timeout for receiving the full request

        # ns_param maxline      8192   ;# default: 8192; maximum size of a single header line
        # ns_param maxheaders   128    ;# default: 128; maximum number of header lines per request
        # ns_param uploadpath   /tmp   ;# default: tmpdir; directory for temporary upload files

        # ns_param maxqueuesize 256    ;# default: 1024; maximum size of the preprocessed request queue

        #------------------------------------------------------------------
        # Socket backlog and accept behaviour
        #------------------------------------------------------------------
        # ns_param backlog       256   ;# default: listenbacklog; listen backlog for this driver;
        #                               # overrides global listenbacklog
        # ns_param acceptsize    10    ;# default: backlog; maximum number of sockets accepted
        #                               # in a single accept loop
        # ns_param sockacceptlog 3     ;# default: sockacceptlog: overrides ns/parameters sockacceptlog

        # Defer accept until data arrives (where supported). This can improve
        # performance but may cause recvwait to be ignored while the socket
        # is still in the kernel’s accept queue.
        # ns_param deferaccept   true  ;# default: false


        #------------------------------------------------------------------
        # Buffers, timeouts, and connection behaviour
        #------------------------------------------------------------------
        # ns_param bufsize       16kB  ;# default: 16kB; size of I/O buffer for reading requests
        # ns_param readahead     16kB  ;# default: bufsize; amount of extra data to read ahead
        # ns_param sendwait      30s   ;# default: 30s; timeout for sending responses
        # ns_param closewait     2s    ;# default: 2s; timeout when closing the socket
        # ns_param keepwait      2s    ;# default: 5s; keep-alive timeout

        # Control TCP_NODELAY (Nagle's algorithm) on accepted sockets.
        #   true  -- disable Nagle (set TCP_NODELAY), good for latency
        #   false -- leave Nagle enabled
        # ns_param nodelay       false ;# default: true

        # ns_param keepalivemaxuploadsize   100kB   ;# default: 0; # 0 = no limit; disable keep-alive
        #                                         ;# for uploads larger than this value
        # ns_param keepalivemaxdownloadsize 1MB   ;# default: 0; 0 = no limit; disable keep-alive
        #                                         ;# for responses larger than this value

        #------------------------------------------------------------------
        # Upload spooling and writer threads
        #------------------------------------------------------------------
        # Spool uploads exceeding this size to a temporary file.
        # 0 = disabled; everything stays in memory.
        # ns_param maxupload          100kB  ;# default: 0; spool request bodies larger than this size

        # Use writer threads for sending large responses.
        # ns_param writerthreads      2      ;# default: 0; number of writer threads (0 = disabled)
        # ns_param writersize         1kB    ;# default: 1MB; use writer threads for responses larger than this size
        # ns_param writerbufsize    16kB   ;# default: 8kB; buffer size for writer threads

        # ns_param writerstreaming  true   ;# default: false; enable writer threads for streaming output (ns_write)
        # ns_param spoolerthreads   1      ;# default: 0; number of upload spooler threads

        #------------------------------------------------------------------
        # Port reuse and driver threading
        #------------------------------------------------------------------
        # Options for port reuse (see https://lwn.net/Articles/542629/).
        # These require OS support and are typically managed automatically
        # when driverthreads > 1.
        #
        # ns_param reuseport      true ;# default: false; normally not set explicitly; enabled when
        #                               # driverthreads > 1 and OS supports SO_REUSEPORT
        # ns_param driverthreads  2    ;# default: 1; number of driver threads; >1 activates reuseport

        #------------------------------------------------------------------
        # Extra response headers
        #------------------------------------------------------------------
        # Extra driver-specific response headers to be added to every
        # HTTP response sent via this driver.
        ns_param extraheaders   $http_extraheaders
    }

    #
    # Register known Host header values for this driver and map them to server
    # configurations.
    #
    ns_section -set $server_set ns/module/http/servers {}
    if {[namespace exists ::docker]} {
        ns_section -set [::docker::map_external_address_to_server $server $httpport] ns/module/http/servers {}
    }
    #ns_log notice ns_configsection [ns_set format [ns_configsection ns/module/http/servers]]
}

#---------------------------------------------------------------------
# Configuration for HTTPS interface (SSL/TLS) -- core module "nsssl"
#---------------------------------------------------------------------

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
        #------------------------------------------------------------------
        # Basic binding and request size limits
        #------------------------------------------------------------------
        # ns_param defaultserver $server
        # ns_param address       $ipaddress
        # ns_param hostname      [lindex $hostname 0]
        ns_param port            $httpsport

        # Per-driver limits for incoming requests; override ns/parameters
        # maxinput/recvwait when set there.
        # ns_param maxinput      $max_file_upload_size      ;# max request body size (e.g. uploads)
        # ns_param recvwait      $max_file_upload_duration  ;# timeout for receiving the full request

        #------------------------------------------------------------------
        # Protocols and TLS configuration
        #------------------------------------------------------------------
        # Server certificate configuration:
        #  - "certificate" points to the main certificate/key for this driver
        #  - "vhostcertificates" is a directory with certificates for
        #    additional virtual hosts of the default server.
        ns_param certificate        $certificate
        ns_param vhostcertificates  $vhostcertificates  ;# directory for vhost certificates of the default server

        # Client certificate verification level (see nsssl docs for details)
        # ns_param verify             0  ;# default: 0

        # Cipher suites for TLS 1.2 and below.
        ns_param ciphers \
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"

        # TLS 1.3 cipher suites (if you want to override OpenSSL defaults).
        # ns_param ciphersuites "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"

        # Enabled/disabled protocol versions.
        ns_param protocols          "!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"

        # OCSP stapling configuration:
        # ns_param OCSPstapling        on   ;# default: off; enable OCSP stapling
        # ns_param OCSPstaplingVerbose on   ;# default: off; more verbose OCSP logging
        # ns_param OCSPcheckInterval   15m  ;# default: 5m; OCSP (re)check interval

        #------------------------------------------------------------------
        # Writer threads and upload handling
        #------------------------------------------------------------------
        # Spool uploads exceeding this size to a temporary file.
        # 0 = disabled; everything stays in memory.
        # ns_param maxupload          100kB  ;# default: 0; spool request bodies larger than this size

        # Use writer threads for sending larger responses.
        # ns_param writerthreads      2      ;# default: 0; number of writer threads (0 = disabled)
        # ns_param writersize         1kB    ;# default: 1MB; use writer threads for responses larger than this size
        # ns_param writerbufsize      16kB   ;# default: 8kB; buffer size for writer threads

        # ns_param writerstreaming  true   ;# default: false; enable writer threads for streaming output (ns_write)
        # ns_param spoolerthreads   1      ;# default: 0; number of upload spooler threads

        #------------------------------------------------------------------
        # Socket and connection behaviour
        #------------------------------------------------------------------
        # ns_param backlog       256   ;# default: listenbacklog; listen backlog for this driver;
        #                               # overrides global listenbacklog
        # ns_param acceptsize    10    ;# default: backlog; maximum number of sockets accepted
        #                               # in a single accept loop
        # ns_param sockacceptlog 3     ;# default: sockacceptlog: overrides ns/parameters sockacceptlog

        # Defer accept until data arrives (where supported). This can improve
        # performance but may influence how recvwait behaves while the socket
        # is still in the kernel’s accept queue.
        # ns_param deferaccept      true   ;# default: false

        # Control TCP_NODELAY (Nagle's algorithm) on accepted sockets.
        #   true  -- disable Nagle (set TCP_NODELAY), lower latency
        #   false -- leave Nagle enabled
        # ns_param nodelay          false  ;# default: true

        #------------------------------------------------------------------
        # Port reuse and driver threading
        #------------------------------------------------------------------
        # Options for port reuse (see https://lwn.net/Articles/542629/).
        # These require OS support and are typically managed automatically
        # when driverthreads > 1.
        #
        # ns_param reuseport      true ;# default: false; normally not set explicitly; enabled when
        #                               # driverthreads > 1 and OS supports SO_REUSEPORT
        # ns_param driverthreads  2    ;# default: 1; number of driver threads; >1 activates reuseport

        #------------------------------------------------------------------
        # Extra response headers
        #------------------------------------------------------------------
        # Extra driver-specific response headers to be added to every HTTPS
        # response sent via this driver.
        ns_param extraheaders	$https_extraheaders
    }

    #
    # Register known Host header values for this driver and map them to server
    # configurations.
    #
    ns_section -set $server_set ns/module/https/servers {}
    if {[namespace exists ::docker]} {
        ns_section -set [::docker::map_external_address_to_server $server $httpsport] ns/module/https/servers {}
    }
}
