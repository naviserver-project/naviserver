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
ns_param   address         127.0.0.1
ns_param   ciphers         "ECDH+AESGCM:DH+AESGCM:ECDH+AES256:DH+AES256:ECDH+AES128:DH+AES:ECDH+3DES:DH+3DES:RSA+AESGCM:RSA+AES:RSA+3DES:!aNULL:!MD5:!RC4"
ns_param   protocols	   "!SSLv2"
ns_param   certificate	   $homedir/etc/server.pem
ns_param   verify     	   0
ns_param   writerthreads   2
ns_param   writersize	   2048

ns_section test
ns_param listenport [ns_config "ns/server/test/module/nsssl" port]
ns_param listenurl https://[ns_config "ns/server/test/module/nsssl" hostname]:[ns_config "ns/server/test/module/nsssl" port]
