# Emacs mode:  -*-Makefile-*-

# This is the Linux/Unix specific file, do NOT put Microsoft Windows
# nmake stuff here.

#
# NOTE: PEM_FILE creation is now part of the server's main Makefile.
#

include Makefile.common
CLEAN += clean-cert


LD_LIBRARY_PATH = LD_LIBRARY_PATH="./:$$LD_LIBRARY_PATH"
NSD             = $(NAVISERVER)/bin/nsd
NS_TEST_CFG     = -c -d -t tests/config.tcl -u nsadmin
NS_TEST_ALL     = all.tcl $(TESTFLAGS)
PEM_FILE	= tests/etc/server.pem

certificate: $(PEM_FILE)

$(PEM_FILE):
	$(OPENSSL) genrsa 2048 > host.key
	$(OPENSSL) req -new -config tests/etc/openssl.cnf -x509 -nodes -sha1 -days 365 -key host.key > host.cert
	$(CAT) host.cert host.key > server.pem
	$(RM) host.cert host.key
	$(OPENSSL) dhparam 1024 >> server.pem
	$(MV) server.pem $(PEM_FILE)

clean-cert:
	rm -f $(PEM_FILE)

test: certificate all
	export $(LD_LIBRARY_PATH); $(NSD) $(NS_TEST_CFG) $(NS_TEST_ALL)
