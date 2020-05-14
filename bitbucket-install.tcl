set pageName [ns_queryget file ""]
switch -exact $pageName {
    nsconf.tcl -
    nsstats.tcl {
        set source https://bitbucket.org/naviserver/[file rootname $pageName]/get/master.tar.gz
        set page [ns_server pagedir]/$pageName
        if {![file readable $page]} {
            set outputfile /tmp/bitbucket-download-[pid].tar.gz
            ns_http run -outputfile $outputfile -spoolsize 0 $source
            if {$tcl_platform(os) eq "Darwin"} {
                set tar "exec tar Ozxvf $outputfile *$pageName > $page"
            } else {
                set tar "exec tar -Ozxvf $outputfile --wildcards *$pageName > $page"
            }
            if {[catch $tar errorMsg]} {
                if {![string match "*.tcl" $errorMsg]} {
                    ns_log notice "error: $errorMsg"
                    ns_return 200 text/html \
                        "<html><body>error while downloading $pageName: <b>$errorMsg</b>. <a href='/'>return</a>"
                    return
                }
            }
            ns_return 200 text/html \
                "<html><body>$pageName successfully downloaded and installed. <a href='/'>return</a>"
            return
        }
        ns_return 200 text/html \
            "<html><body>$pageName already installed. <a href='/'>return</a>"
    }
    default {
        set pageName [string map [list < "&lt;" > "&gt;"] $pageName]
        ns_return 200 text/html "<html><body>page $pageName unknown. <a href='/'>return</a>"
    }
}

#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
