#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
#
#
#
MAN_CSS=man-5.0.css
HEADER_INC=header-5.0.inc

NSBUILD=1
include include/Makefile.global

dirs    = nsthread nsd nssock nscgi nscp nslog nsperm nsdb nsdbtest nsssl revproxy

# Unix only modules
ifeq (,$(findstring MINGW,$(uname)))
   dirs += nsproxy
endif

distfiles = $(dirs) doc tcl contrib include tests win win32 configure m4 \
	Makefile autogen.sh install-sh missing aclocal.m4 configure.ac \
	config.guess config.sub \
	README.md NEWS sample-config.tcl.in simple-config.tcl openacs-config.tcl \
	nsd-config.tcl index.adp license.terms naviserver.rdf naviserver.rdf.in \
	version_include.man.in install-from-repository.tcl

all:
	@for i in $(dirs); do \
		( cd $$i && $(MAKE) all ) || exit 1; \
	done
	@if [ -n "${PEM_FILE}" ]; then \
		$(MAKE) $(PEM_FILE) ; \
	fi

help:
	@echo 'Commonly used make targets:'
	@echo '  all          - build program and documentation'
	@echo '  install      - install program and man pages under $(NAVISERVER)'
	@echo '  test         - run all tests in the automatic test suite'
	@echo '  gdbtest      - run all tests, under the control of the debugger'
	@echo '  runtest      - start the server in interactive command mode'
	@echo '  gdbruntest   - start the server in command mode, under the debugger'
	@echo '  memcheck     - run all tests, under the valgrind memory checker'
	@echo '  build-doc    - build the HTML and nroff documentation'
	@echo '  dist         - create a source tarball naviserver-'$(NS_PATCH_LEVEL)'.tar.gz'
	@echo '  clean        - remove files created by other targets'
	@echo
	@echo 'Example for a system-wide installation under /usr/local/ns:'
	@echo '  make all && su -c "make install"'
	@echo
	@echo 'Example for running selected test in the test suite, under the debugger:'
	@echo '  make gdbtest TESTFLAGS="-verbose start -file cookies.test -match cookie-2.*"'
	@echo

install: install-dirs install-include install-tcl install-modules \
	install-config install-certificates install-doc install-examples install-notice

HAVE_NSADMIN := $(shell id -u nsadmin 2> /dev/null)

install-notice:
	@echo ""
	@echo ""
	@echo "Congratulations, you have installed NaviServer."
	@echo ""
	@if [ $(shell id -u) = 0 ]; then \
	    if [ "x${HAVE_NSADMIN}" = "x" ] ; then \
		echo "  When running as root, the server needs an unprivileged user to be"; \
		echo "  specified (e.g. nsadmin). This user can be created on a Linux-like system with"; \
		echo "  the command"; \
		echo ""; \
		echo "  useradd nsadmin"; \
		echo ""; \
	    else \
		if [ ! `sudo -u nsadmin test -w $(NAVISERVER)/logs && echo 1` ] ; then \
		    echo "The permissions for log directory have to be set up:"; \
		    echo ""; \
		    echo "  chown -R nsadmin:nsadmin $(NAVISERVER)/logs"; \
		    echo ""; \
		fi; \
	    fi; \
	    user="-u nsadmin"; \
	fi; \
	echo "You can now run NaviServer by typing the following command: "; \
	echo ""; \
	echo "  $(NAVISERVER)/bin/nsd $$user -t $(NAVISERVER)/conf/nsd-config.tcl -f"; \
	echo ""; \
	echo "As a next step, you need to configure the server according to your needs."; \
	echo "Consult the sample configuration files in $(NAVISERVER)/conf/ as a reference."; \
	echo ""

install-dirs: all
	@for i in bin lib logs include tcl pages conf certificates modules modules/tcl cgi-bin; do \
		$(MKDIR) $(DESTDIR)$(NAVISERVER)/$$i; \
	done

install-config: all
	@$(MKDIR) $(DESTDIR)$(NAVISERVER)/conf $(DESTDIR)$(NAVISERVER)/pages/
	@for i in returnnotice.adp nsd-config.tcl sample-config.tcl simple-config.tcl openacs-config.tcl ; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/conf/; \
	done
	@for i in index.adp install-from-repository.tcl; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/pages/; \
	done
	$(INSTALL_SH) install-sh $(DESTDIR)$(INSTBIN)/

install-certificates: $(PEM_FILE) ca-bundle.crt
	@$(MKDIR) $(DESTDIR)$(NAVISERVER)/certificates
	@$(MKDIR) $(DESTDIR)$(NAVISERVER)/invalid-certificates
	@if [ -f "$(DESTDIR)$(NAVISERVER)/etc" ]; then \
		for i in `ls $(DESTDIR)$(NAVISERVER)/etc/*pem` ; do \
			$(LN) -sf $$i $(DESTDIR)$(NAVISERVER)/certificates ; \
		done; \
	fi
	for i in `ls ./certificates/*` ; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/certificates/; \
	done
	@if [ -n "$(OPENSSL_LIBS)" ]; then \
		$(OPENSSL) rehash $(DESTDIR)$(NAVISERVER)/certificates ; \
	fi
	$(INSTALL_DATA) ca-bundle.crt $(DESTDIR)$(NAVISERVER)/

install-modules: all
	@for i in $(dirs); do \
		(cd $$i && $(MAKE) install) || exit 1; \
	done

install-tcl: all
	@for i in tcl/*.tcl; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/tcl/; \
	done

install-include: all
	@for i in include/*.h include/Makefile.global include/Makefile.module; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(INSTHDR)/; \
	done

install-tests:
	$(CP) tests $(INSTSRVPAG)

install-doc:
	@if [ -d doc/html ]; then \
		$(MKDIR) $(DESTDIR)$(NAVISERVER)/pages/doc ; \
		$(MKDIR) $(DESTDIR)$(NAVISERVER)/pages/doc/naviserver ; \
		$(CP) doc/html/* $(DESTDIR)$(NAVISERVER)/pages/doc ; \
		$(CP) contrib/banners/*.png $(DESTDIR)$(NAVISERVER)/pages/doc ; \
		$(CP) doc/src/$(MAN_CSS) $(DESTDIR)$(NAVISERVER)/pages/doc/naviserver/ ; \
		echo "\nThe documentation is installed under: $(DESTDIR)$(NAVISERVER)/pages/doc" ; \
	else \
		echo "\nNo documentation is installed locally; either generate the documentation with" ; \
		echo "   make build-doc"; \
		echo "(which requires tcllib to be installed, such that dtplite can be used for the generation)" ; \
		echo "or use the online documentation from https://naviserver.sourceforge.io/n/toc.html" ; \
	fi;

install-examples:
	@$(MKDIR) $(DESTDIR)$(NAVISERVER)/pages/examples
	@for i in contrib/examples/*.adp contrib/examples/*.tcl; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/pages/examples/; \
	done


# On some systems you may need a special shell script to control the
# PATH seen by dtplite, as that influences which versions of tclsh and
# Tcllib dtplite uses.  I personally found it necessary to use a
# one-line script like this for the DTPLITE command:
#   env PATH=/usr/sbin:/usr/bin:/sbin:/bin dtplite "$@"
# --atp@piskorski.com, 2014/08/27 10:52 EDT

ifeq ($(DTPLITE),)
  DTPLITE=dtplite
else
  # Do nothing, use the environment variable as is.
endif

build-doc:
	$(RM) doc/html doc/man doc/tmp
	$(MKDIR) doc/html doc/man doc/tmp
	@for srcdir in nscgi \
		       nslog \
		       nsdb \
		       nsproxy \
		       nscp \
		       nsperm \
		       nssock \
		       nsssl \
		       revproxy \
		       doc/src/manual \
		       doc/src/naviserver \
		       modules/nsexpat \
		       modules/nsconfigrw \
		       modules/nsdbi \
		       modules/nsloopctl \
		       modules/nsvfs; do \
		if [ -d $$srcdir ]; then \
		   echo $$srcdir; \
		   $(MKDIR) doc/tmp/`basename $$srcdir`; \
		   find $$srcdir -name '*.man' -exec $(CP) "{}" doc/tmp/`basename $$srcdir` ";"; \
		fi; \
	done
	$(CP) doc/images/manual/*.png doc/tmp/manual/
	$(CP) doc/images/naviserver/*.png doc/tmp/naviserver/
	$(CP) revproxy/doc/mann/*.png doc/tmp/revproxy/
	$(CP) doc/commandlist_include.man doc/tmp/
	@cd doc/tmp; \
	for srcdir in `ls`; do \
	    echo $$srcdir; \
	    if [ -f $$srcdir/version_include.man ]; then \
	       $(CP) $$srcdir/version_include.man .; \
	    else \
	       $(CP) ../../version_include.man .; \
	    fi; \
	    echo $(DTPLITE) -merge -style ../src/$(MAN_CSS) \
		       -header ../src/$(HEADER_INC) \
		       -footer ../src/footer.inc \
		       -o ../html/ html $$srcdir; \
	    $(DTPLITE) -merge -style ../src/$(MAN_CSS) \
		       -header ../src/$(HEADER_INC) \
		       -footer ../src/footer.inc \
		       -o ../html/ html $$srcdir; \
	    $(DTPLITE) -merge -o ../man/ nroff $$srcdir; \
	done
	$(RM) doc/tmp

#
# Testing:
#

ifeq ($(shell id -u),0)
NS_TEST_CFG	= -u nsadmin -c -d -t $(srcdir)/tests/test.nscfg
else
NS_TEST_CFG	= -c -d -t $(srcdir)/tests/test.nscfg
endif


NS_TEST_ALL	= $(srcdir)/tests/all.tcl $(TESTFLAGS)
NS_LD_LIBRARY_PATH	= \
   LD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$LD_LIBRARY_PATH" \
   DYLD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$DYLD_LIBRARY_PATH"

EXTRA_TEST_DIRS =
ifneq ($(OPENSSL_LIBS),)
  #EXTRA_TEST_DIRS += nsssl
  TEST_CERTIFICATES = tests/testserver/certificates
  PEM_FILE          = $(TEST_CERTIFICATES)/server.pem
  PEM_PRIVATE       = $(TEST_CERTIFICATES)/myprivate.pem
  PEM_PUBLIC        = $(TEST_CERTIFICATES)/mypublic.pem
  SSLCONFIG         = $(TEST_CERTIFICATES)/openssl.cnf
  EXTRA_TEST_REQ    = $(PEM_FILE)
endif

$(PEM_FILE): $(PEM_PRIVATE)
	$(OPENSSL) genrsa 2048 > host.key
	# openssl rejects on some platforms building certificates with SHA1, which requires TLS>1.0, excluding Windows XP.
	$(OPENSSL) req -new -config $(SSLCONFIG) -x509 -nodes -sha256 -days 365 -key host.key > host.cert
	$(CAT) host.cert host.key > server.pem
	$(RM) -rf host.cert host.key
	$(OPENSSL) dhparam 1024 >> server.pem
	$(MKDIR) certificates
	$(CP) server.pem certificates/
	$(MV) server.pem $(PEM_FILE)
	($(OPENSSL) rehash $(TEST_CERTIFICATES) 2>/dev/null || true)

$(PEM_PRIVATE):
	$(OPENSSL) genrsa -out $(PEM_PRIVATE) 512
	$(OPENSSL) rsa -in $(PEM_PRIVATE) -pubout > $(PEM_PUBLIC)
	$(CHMOD) 644 $(PEM_PRIVATE)

check: test

test: all $(EXTRA_TEST_REQ)
	@if [ $(shell id -u) = 0 ]; then \
	    $(CHOWN) -R nsadmin $(srcdir)/tests ; \
	fi;
	$(NS_LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
	@for i in $(EXTRA_TEST_DIRS); do \
		( cd $$i && $(MAKE) test ) || exit 1; \
	done

runtest: all
	$(NS_LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG)

gdbtest: all
	$(NS_LD_LIBRARY_PATH) gdb -ex=run --args ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
#	@echo set args $(NS_TEST_CFG) $(NS_TEST_ALL) > gdb.run
#	$(NS_LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
#	$(RM) gdb.run

lldbtest: all
	$(NS_LD_LIBRARY_PATH) lldb -- ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

lldb-sample: all
	lldb -o run -- $(DESTDIR)$(NAVISERVER)/bin/nsd -f -u nsadmin -t $(DESTDIR)$(NAVISERVER)/conf/nsd-config.tcl


gdbruntest: all
	@echo set args $(NS_TEST_CFG) > gdb.run
	$(NS_LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
	$(RM) gdb.run

memcheck: all
	$(NS_LD_LIBRARY_PATH) valgrind --tool=memcheck ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
helgrind: all
	$(NS_LD_LIBRARY_PATH) valgrind --tool=helgrind ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

CPPCHECK_SYS_INCLUDES=-I/usr/include
#CPPCHECK_SYS_INCLUDES=-I`xcrun --show-sdk-path`/usr/include

cppcheck:
	$(CPPCHECK) --verbose --inconclusive -j4 --enable=all --checkers-report=cppcheck.txt --check-level=exhaustive \
		nscp/*.c nscgi/*.c nsd/*.c nsdb/*.c nsproxy/*.c nssock/*.c nsperm/*.c nsssl/*.c \
		-I./include $(CPPCHECK_SYS_INCLUDES) -D__x86_64__ -DNDEBUG $(DEFS)

CLANG_TIDY_CHECKS=
#CLANG_TIDY_CHECKS=-checks=-*,performance-*,portability-*,cert-*,modernize-*
#CLANG_TIDY_CHECKS=-checks=-*,modernize-*,performance-*,portability-*,cert-*
#CLANG_TIDY_CHECKS=-checks=-*,bugprone-*
clang-tidy:
	clang-tidy-mp-19 nscp/*.c nscgi/*.c nsd/*.c nsdb/*.c nsproxy/*.c nssock/*.c nsperm/*.c \
		$(CLANG_TIDY_CHECKS) -- \
		-I./include -I/usr/include $(DEFS)

checkexports: all
	@for i in $(dirs); do \
		nm -p $$i/*${LIBEXT} | awk '$$2 ~ /[TDB]/ { print $$3 }' | sort -n | uniq | grep -v '^[Nn]s\|^TclX\|^_'; \
	done

clean:
	@for i in $(dirs); do \
		(cd $$i && $(MAKE) clean) || exit 1; \
	done

clean-bak: clean
	@find . -name '*~' -exec rm "{}" ";"

distclean: clean
	$(RM) config.status config.log config.cache autom4te.cache aclocal.m4 configure \
		include/{Makefile.global,Makefile.module,config.h,config.h.in,stamp-h1} \
		naviserver-$(NS_PATCH_LEVEL).tar.gz sample-config.tcl \
		$(PEM_FILE) $(PEM_PRIVATE) $(PEM_PUBLIC)

config.guess:
	wget -O config.guess 'https://git.savannah.gnu.org/gitweb/?p=config.git;a=blob_plain;f=config.guess;hb=HEAD'
config.sub:
	wget -O config.sub 'https://git.savannah.gnu.org/gitweb/?p=config.git;a=blob_plain;f=config.sub;hb=HEAD'
ca-bundle.crt:
	wget -O ca-bundle.crt 'https://raw.githubusercontent.com/bagder/ca-bundle/refs/heads/master/ca-bundle.crt'

dist: config.guess config.sub clean
	$(RM) naviserver-$(NS_PATCH_LEVEL)
	$(MKDIR) naviserver-$(NS_PATCH_LEVEL)
	$(CP) $(distfiles) naviserver-$(NS_PATCH_LEVEL)
	$(RM) naviserver-$(NS_PATCH_LEVEL)/include/{config.h,Makefile.global,Makefile.module,stamp-h1}
	$(RM) naviserver-$(NS_PATCH_LEVEL)/*/*-{debug,gn}
	$(RM) naviserver-$(NS_PATCH_LEVEL)/tests/testserver/access.log
	git log --date-order --name-status --date=short  >naviserver-$(NS_PATCH_LEVEL)/ChangeLog
	if [ -f $(HOME)/scripts/fix-typos.tcl ]; then \
		(cd naviserver-$(NS_PATCH_LEVEL)/; tclsh $(HOME)/scripts/fix-typos.tcl -name Change\*) \
	fi;
	find naviserver-$(NS_PATCH_LEVEL) -type f -name '.[a-zA-Z_]*' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*-original' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '[a-z]*.pem' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*.c-*' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*.h-*' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*~' -exec rm \{} \;
	tar czf naviserver-$(NS_PATCH_LEVEL).tar.gz --no-xattrs --disable-copyfile --exclude="._*" naviserver-$(NS_PATCH_LEVEL)
	$(RM) naviserver-$(NS_PATCH_LEVEL)


.PHONY: all install clean distclean \
	install-dirs install-include install-tcl install-modules \
	install-config install-certificates install-doc install-examples install-notice
