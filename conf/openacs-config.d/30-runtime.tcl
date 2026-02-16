######################################################################
# Section 3 -- Global runtime configuration (threads, MIME types, fastpath)
######################################################################

#---------------------------------------------------------------------
# Thread library (nsthread) parameters
#---------------------------------------------------------------------
ns_section ns/threads {
    ns_param	stacksize	1MB
}

#---------------------------------------------------------------------
# Extra mime types
#---------------------------------------------------------------------
ns_section ns/mimetypes {
    #  Note: NaviServer already has an exhaustive list of MIME types:
    #  see: /usr/local/src/naviserver/nsd/mimetypes.c
    #  but in case something is missing you can add it here.

    #ns_param	default		*/*
    #ns_param	noextension	text/html
    #ns_param	.pcd		image/x-photo-cd
    #ns_param	.prc		application/x-pilot
}

#---------------------------------------------------------------------
# Global fastpath parameters
#---------------------------------------------------------------------
ns_section ns/fastpath {
    #ns_param        cache               true       ;# default: false
    #ns_param        cachemaxsize        10MB       ;# default: 10MB
    #ns_param        cachemaxentry       100kB      ;# default: 8kB
    #ns_param        mmap                true       ;# default: false
    #ns_param        gzip_static         true       ;# default: false; check for static gzip file
    #ns_param        gzip_refresh        true       ;# default: false; refresh stale .gz files
    #                                                #on the fly using ::ns_gzipfile
    #ns_param        gzip_cmd            "/usr/bin/gzip -9"  ;# use for re-compressing
    #ns_param        minify_css_cmd      "/usr/bin/yui-compressor --type css"
    #ns_param        minify_js_cmd       "/usr/bin/yui-compressor --type js"
    #ns_param        brotli_static       true       ;# default: false; check for static brotli files
    #ns_param        brotli_refresh      true       ;# default: false; refresh stale .br files
    #                                                # on the fly using ::ns_brotlifile
    #ns_param        brotli_cmd          "/usr/bin/brotli -f -Z"  ;# use for re-compressing
}
