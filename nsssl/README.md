# SSL driver for NaviServer 4.99.12 #

## Release 2.0 ##

    vlad@crystalballinc.com
    neumann@wu-wien.ac.at

This is NaviServer module that implements HTTP over SSL support and
adds new ns_ssl command.

* New in Version 0.2:
    - Made driver fully asynchronous, supporting partial read and writes
    - Performance improvements: E.g. support for TCP_CORK and deferaccept
    - Strengthen security against BEAST and CRIME attack

* New in Version 0.3:
    - ns_ssl working with asynchronous I/O
    - ns_ssl: spooling of large content to a file instead of memory
    - requires NaviServer 4.99.6

* New in Version 0.4:
    - Support for forward secrecy + Diffie-Hellmann key exchange.
      If the *DHE* ciphers are to be used, the Diffie-Hellman parameters
      should be appended to the certificate file (e.g. "server.pem",
      see sample below)

* New in Version 0.5:
    - Support for Elliptic Curve Cryptography
      (such as Elliptic Curve Diffie-Hellman (ECDH))
    - Provide compiled-in defaults for DH parameters
    - Handling several SSL and TLS bugs.
    - Deactivated SSLv2

* New in Version 0.6:
    - Ability to load the module without listening to socket by specifying 0 as port.
      This is useful for e.g. just using the HTTPS client command ns_ssl.

* New in Version 0.7:
    - Fixed a bug with in https client commands (ns_ssl) when paths and parameters
      are passed.

* New in Version 0.8:
    - Added regression test infrastructure (nstest::https and test server
	  setup) and test cases

* New in Version 1.0:
    - Support for IPv6

* New in Version 1.1:
    - Support for "-body_file" in ns_ssl.

* New in Version 2.0:
    - OpenSSL support in core.

* New in Version 2.1:
    - Support for client-side Server Name Indication (SNI).
***

## Configuration: ##

   nsd.tcl

     ns_section    ns/server/${servername}/modules
     ns_param      nsssl        		nsssl.so

     ns_section    ns/server/${servername}/module/nsssl
     ns_param	   certificate 		/usr/local/ns/modules/nsssl/server.pem
     ns_param      address    		0.0.0.0
     ns_param      port       		443
     ns_param      ciphers              "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:DHE-RSA-CHACHA20-POLY1305"
     ns_param      protocols            "!SSLv2:!SSLv3:!TLSv1.0:!TLSv1.1"
     ns_param      verify                0

     ns_param      extraheaders {
        Strict-Transport-Security "max-age=31536000; includeSubDomains"
        X-Frame-Options SAMEORIGIN
        X-Content-Type-Options nosniff
     }


 * The parameter "certificate" is required, nsssl won't load without it;
   the .pem file should contain cert and privkey, and could contain DH parameters.

 * The parameter "ciphers" defines which ciphers will be used, by default nsssl uses all ciphers;
   see e.g.: https://wiki.mozilla.org/Security/Server_Side_TLS

 * The parameter "protocols" defines which protocols are enabled;
   by default all protocols are enabled.

 * If the parameter "verify" is set to 1, nsssl will reject any connections without
   valid ceritificate.

 * The parameter "extraheaders" specifies, which headers should be sent on every request.
   The example above, HTTP Strict Transport Security (HSTS) is enabled.


 All other driver related parameters if the HTTP driver can be specified
 (see nssock for more details).

 Creating self-signed certificate
 (The last line is optional but necessary perfect forward secrecy)

    openssl genrsa 1024 > host.key
    openssl req -new -x509 -nodes -sha1 -days 365 -key host.key > host.cert
    cat host.cert host.key > server.pem
    rm -rf host.cert host.key
    openssl dhparam 2048 >> server.pem


## Compile and Install: ##

   To compile and install one might use commands like the following
   (assuming that NaviServer was compiled with openssl support)
	
    make
    make install

## Authors: ##

    Vlad Seryakov vlad@crystalballinc.com
    Gustaf Neumann neumann@wu-wien.ac.at
