# 
# This script is to be called from NaviServer and handles package
# installation requests based on the named provided via the query
# variable "package". This file is intended only for pure Tcl modules.
#
# For the recognized package "nsstats":
#  1. It sets the source URL for downloading the package tarball from GitHub
#
#  2. It constructs the destination file path (page) in the server's
#     page directory.
#
#  3. It checks if the package file already exists (is readable). If
#     not: It downloads the package tarball to a temporary output
#     file, extracts it and install the required files (currently to
#     the root directory).
#
#  4. After installation, it returns an HTML success message.
#
# For any unrecognized package name (default case):
#  - It sanitizes the package name by mapping certain characters and
#    returns an HTML message indicating that the requested page is
#    unknown.

set packageName [ns_queryget package ""]
switch -exact $packageName {
    nsstats {
        #set source https://bitbucket.org/naviserver/$packageName/get/main.tar.gz
        set source https://codeload.github.com/naviserver-project/$packageName/tar.gz/refs/heads/main
        set page [ns_server pagedir]/$packageName.tcl
        if {![file readable $page]} {
            set outputfile /tmp/bitbucket-download-[pid].tar.gz
            ns_http run -outputfile $outputfile -spoolsize 0 $source
            if {$::tcl_platform(os) eq "Darwin"} {
                set tar "exec tar Ozxvf $outputfile *$packageName > $page"
            } else {
                set tar "exec tar -Ozxvf $outputfile --wildcards *$packageName > $page"
            }
            set tar "exec tar zxf $outputfile -C /tmp"
            if {[catch $tar errorMsg]} {
                if {![string match "*.tcl" $errorMsg]} {
                    ns_log notice "error: $errorMsg"
                    ns_return 200 text/html \
                        "<html><body>error while downloading $packageName: <b>$errorMsg</b>. <a href='/'>return</a>"
                    return
                }
            } else {
                foreach file {nsstats.tcl nsstats.adp nsstats-4.99.adp} {
                    if {[file readable /tmp/nsstats-main/$file]} {
                        ns_log notice "installing [ns_server pagedir]/$file"
                        file rename -force /tmp/nsstats-main/$file [ns_server pagedir]/$file
                    }
                }
            }
            # There is currently a templating problem with plain naviserver installations and "ns_returnredirect"
            #ns_returnredirect /
            ns_set update [ns_conn outputheaders] location /
            ns_return 302 text/plain ""
            return
        }
        ns_return 200 text/html "<html><body>$packageName already installed. <a href='/'>return</a>"
    }
    default {
        set packageName [string map [list < "&lt;" > "&gt;"] $packageName]
        ns_return 200 text/html "<html><body>page $packageName unknown. <a href='/'>return</a>"
    }
}

#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
