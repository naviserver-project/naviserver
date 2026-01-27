########################################################################
#
# Section 2 -- Global network drivers & modules
#
########################################################################

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

    ns_section ns/module/http {
        ns_param defaultserver  default
        ns_param port           $httpport
        ns_param address        $ipaddress  ;# Space separated list of IP addresses
        #ns_param hostname      [ns_info hostname]

        #ns_param backlog       1024         ;# default: 256; backlog for listen operations
        #ns_param acceptsize    10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
        #ns_param sockacceptlog 3            ;# ns/param sockacceptlog; report, when one accept operation
                                             ;# receives more than this threshold number of sockets
        #ns_param closewait     0s           ;# default: 2s; timeout for close on socket
        #ns_param keepwait      5s           ;# 5s, timeout for keep-alive
        #ns_param maxqueuesize  1024         ;# default: 1024; maximum size of the queue

        #ns_param readahead     1MB          ;# default: 16384; size of readahead for requests
        #ns_param maxupload     1MB          ;# default: 0, when specified, spool uploads larger than this value to a temp file

        ns_param maxinput       $max_file_upload_size      ;# Maximum file size for uploads
        ns_param recvwait       $max_file_upload_duration  ;# 30s, timeout for receive operations
        #ns_param keepalivemaxuploadsize   0.5MB           ;# 0, don't allow keep-alive for upload content larger than this
        #ns_param keepalivemaxdownloadsize 1MB             ;# 0, don't allow keep-alive for download content larger than this

        #
        # Spooling Threads
        #
        ns_param  writerthreads   1           ;# default: 0, number of writer threads
        #ns_param writersize      1KB         ;# default: 1MB, use writer threads for files larger than this value
        #ns_param writerbufsize   16kB        ;# default: 8kB, buffer size for writer threads
        #ns_param writerstreaming true        ;# false;  activate writer for streaming HTML output (e.g. ns_writer)

        #ns_param spoolerthreads  1           ;# default: 0; number of upload spooler threads
        #ns_param driverthreads   2           ;# default: 1, number of driver threads (requires support of SO_REUSEPORT)

        #
        # TCP tuning
        #
        #ns_param nodelay         false       ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        #ns_param deferaccept     true        ;# false, Performance optimization

        #
        # Extra response header fields for this driver
        #
        ns_param extraheaders    $http_extraheaders
    }

    #
    # Define, which "host" (as supplied by the "host:" header field)
    # accepted over this driver should be associated with which server.
    #
    ns_section ns/module/http/servers {
        foreach domainname $hostname {
            ns_param default $domainname
        }
        ns_param default [ns_info hostname]
        foreach address $ipaddress {
            ns_param default $address
        }
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

    ns_section ns/module/https {
        ns_param defaultserver  default
        ns_param port           $httpsport
        ns_param address        $ipaddress
        #ns_param hostname      [ns_info hostname]

        #ns_param backlog       1024         ;# default: 256; backlog for listen operations
        #ns_param acceptsize    10           ;# default: value of "backlog"; max number of accepted (but unqueued) requests
        #ns_param sockacceptlog 3            ;# ns/param sockacceptlog; report, when one accept operation
                                             ;# receives more than this threshold number of sockets
        #ns_param closewait     0s           ;# default: 2s; timeout for close on socket
        #ns_param keepwait      5s           ;# 5s, timeout for keep-alive
        #ns_param maxqueuesize  1024         ;# default: 1024; maximum size of the queue

        #ns_param readahead     1MB          ;# default: 16384; size of readahead for requests
        #ns_param maxupload     1MB          ;# default: 0, when specified, spool uploads larger than this value to a temp file

        ns_param maxinput       $max_file_upload_size      ;# Maximum file size for uploads
        ns_param recvwait       $max_file_upload_duration  ;# 30s, timeout for receive operations
        #ns_param keepalivemaxuploadsize   0.5MB           ;# 0, don't allow keep-alive for upload content larger than this
        #ns_param keepalivemaxdownloadsize 1MB             ;# 0, don't allow keep-alive for download content larger than this

        #
        # Spooling Threads
        #
        ns_param  writerthreads   1           ;# default: 0, number of writer threads
        #ns_param writersize      1KB         ;# default: 1MB, use writer threads for files larger than this value
        #ns_param writerbufsize   16kB        ;# default: 8kB, buffer size for writer threads
        #ns_param writerstreaming true        ;# false;  activate writer for streaming HTML output (e.g. ns_writer)

        #ns_param spoolerthreads  1           ;# default: 0; number of upload spooler threads
        #ns_param driverthreads   2           ;# default: 1, number of driver threads (requires support of SO_REUSEPORT)

        #
        # TCP tuning
        #
        #ns_param nodelay         false       ;# true; deactivate TCP_NODELAY if Nagle algorithm is wanted
        #ns_param deferaccept     true        ;# false, Performance optimization

        #
        # SSL/TLS parameters
        #
        ns_param ciphers         "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"
        #ns_param ciphersuites   "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"
        ns_param protocols       "!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"
        ns_param certificate     $certificate
        #ns_param vhostcertificates $home/etc/certificates ;# directory for vhost certificates of the default server
        ns_param verify          0

        ns_param extraheaders    $https_extraheaders
        ns_param OCSPstapling    on          ;# off; activate OCSP stapling
        # ns_param OCSPstaplingVerbose  on    ;# off; make OCSP stapling more verbose
        # ns_param OCSPcheckInterval 15m      ;# default 5m; OCSP (re)check intervale
    }

    #
    # Define, which "host" (as supplied by the "host:" header field)
    # accepted over this driver should be associated with which server.
    #
    ns_section ns/module/https/servers {
        foreach domainname $hostname {
            ns_param default $domainname
        }
        ns_param default [ns_info hostname]
        foreach address $ipaddress {
            ns_param default $address
        }
    }
}

#
# The following section defines, which hostnames map to which
# server. In our case for example, the host "localhost" is mapped to
# the nsd server named "default".
#
# (Kept here as a minimal virtual-host sample; note that the earlier
#  ns/module/http/servers block already registers $hostname and $ipaddress.)
#
ns_section ns/module/http/servers {
    ns_param default    localhost
    ns_param default    [ns_info hostname]
}
