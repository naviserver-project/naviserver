########################################################################
#
# Section 3 -- Global runtime configuration
#
########################################################################

ns_section ns/threads {
    ns_param stacksize 512kB
}

ns_section ns/mimetypes {
    ns_param default     text/plain
    ns_param noextension text/plain
}

ns_section ns/fastpath {
    #ns_param cache          false      ;# default: false
    #ns_param cachemaxsize   10MB       ;# default: 10MB
    #ns_param cachemaxentry  8kB        ;# default: 8kB
    #ns_param mmap           false      ;# default: false
    ns_param gzip_static    true        ;# check for static gzip; default: false
    ns_param gzip_refresh   true        ;# refresh stale .gz files on the fly using ::ns_gzipfile
    ns_param gzip_cmd       "/usr/bin/gzip -9"  ;# use for re-compressing
    ns_param brotli_static  true        ;# check for static brotli files; default: false
    ns_param brotli_refresh true        ;# refresh stale .br files on the fly using ::ns_brotlifile
    ns_param brotli_cmd     "/usr/bin/brotli -f -Z"  ;# use for re-compressing
    #ns_param brotli_cmd     "/opt/local/bin/brotli -f -Z"  ;# use for re-compressing (macOS + ports)
}

ns_section ns/servers {
    ns_param default $serverprettyname
}
