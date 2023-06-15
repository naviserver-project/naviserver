#
# nsssl configuration test.
#

set homedir [pwd]/tests
set bindir  [file dirname [ns_info nsd]]

ns_section "ns/parameters"
ns_param   home           $homedir
ns_param   tcllibrary     $bindir/../tcl
ns_param   logdebug       false

ns_section "ns/servers"
ns_param   test            "Test Server"

ns_section "ns/server/test/tcl"
ns_param   initfile        $bindir/init.tcl
ns_param   library         $homedir/modules

ns_section "ns/server/test/modules"
ns_param   nsssl           [pwd]/nsssl.so

ns_section "ns/server/test/module/nsssl"
ns_param   port            8443
ns_param   hostname        localhost
ns_param   address         [expr {[ns_info ipv6] ? "::1" : "127.0.0.1"}]
ns_param   ciphers         "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"
ns_param   protocols	   "!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"
ns_param   certificate	   $homedir/etc/server.pem
ns_param   verify     	   0
ns_param   writerthreads   2
ns_param   writersize	   2048

ns_section test
ns_param listenport [ns_config "ns/server/test/module/nsssl" port]
ns_param listenurl https://\[[ns_config "ns/server/test/module/nsssl" address]\]:[ns_config "ns/server/test/module/nsssl" port]
