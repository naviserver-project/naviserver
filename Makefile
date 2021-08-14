#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://www.mozilla.org/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is AOLserver Code and related documentation
# distributed by AOL.
#
# The Initial Developer of the Original Code is America Online,
# Inc. Portions created by AOL are Copyright (C) 1999 America Online,
# Inc. All Rights Reserved.
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
#
#
#

NSBUILD=1
include include/Makefile.global

dirs   = nsthread nsd nssock nscgi nscp nslog nsperm nsdb nsdbtest nsssl

# Unix only modules
ifeq (,$(findstring MINGW,$(uname)))
   dirs += nsproxy
endif

distfiles = $(dirs) doc tcl contrib include tests win win32 configure m4 \
	Makefile autogen.sh install-sh missing aclocal.m4 configure.ac \
	config.guess config.sub \
	README NEWS sample-config.tcl.in simple-config.tcl openacs-config.tcl \
	nsd-config.tcl index.adp license.terms naviserver.rdf naviserver.rdf.in \
	version_include.man.in bitbucket-install.tcl

all:
	@for i in $(dirs); do \
		( cd $$i && $(MAKE) all ) || exit 1; \
	done

help:
	@echo 'Commonly used make targets:'
	@echo '  all          - build program and documentation'
	@echo '  install      - install program and man pages under $(NAVISERVER)'
	@echo '  test         - run all tests in the automatic test suite'
	@echo '  gdbtest      - run all tests, under the control of the debugger'
	@echo '  runtest      - start the server in interactive command mode'
	@echo '  gdbruntest   - start the server in command mode, under the debugger'
	@echo '  memcheck     - run all tests, under the valgrind memory checker'
	@echo '  build-doc    - build the html and nroff documentation'
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
	install-config install-doc install-examples install-notice

install-notice:
	@echo ""
	@echo ""
	@echo "Congratulations, you have installed NaviServer."
	@echo ""
	@if [ "`whoami`" = "root" ]; then \
	  echo "  Because you are running as root, the server needs an unprivileged user to be"; \
	  echo "  specified (e.g. nsadmin). This user can be created on a Linux-like system with"; \
	  echo "  the command"; \
	  echo ""; \
	  echo "  useradd nsadmin"; \
	  echo ""; \
	  echo "The permissions for log directory have to be set up:"; \
	  echo ""; \
	  echo "  chown -R nsadmin:nsadmin $(NAVISERVER)/logs"; \
	  echo ""; \
	  user="-u nsadmin"; \
	fi; \
	echo "You can now run NaviServer by typing the following command: "; \
	echo ""; \
	echo "  $(NAVISERVER)/bin/nsd $$user -t $(NAVISERVER)/conf/nsd-config.tcl -f"; \
	echo ""; \
	echo "As a next step, you need to configure the server according to your needs."; \
	echo "Consult as a reference the alternate configuration files in $(NAVISERVER)/conf/"; \
	echo ""

install-dirs: all
	@for i in bin lib logs include tcl pages conf modules modules/tcl cgi-bin; do \
		$(MKDIR) $(DESTDIR)$(NAVISERVER)/$$i; \
	done

install-config: all
	@mkdir -p $(DESTDIR)$(NAVISERVER)/conf $(DESTDIR)$(NAVISERVER)/pages/
	@for i in nsd-config.tcl sample-config.tcl simple-config.tcl openacs-config.tcl ; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/conf/; \
	done
	@for i in index.adp bitbucket-install.tcl; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/pages/; \
	done
	$(INSTALL_SH) install-sh $(DESTDIR)$(INSTBIN)/

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
		$(CP) doc/src/man.css $(DESTDIR)$(NAVISERVER)/pages/doc/naviserver/ ; \
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
	@cd doc/tmp; \
	for srcdir in `ls`; do \
	    echo $$srcdir; \
            if [ -f $$srcdir/version_include.man ]; then \
               $(CP) $$srcdir/version_include.man .; \
	    else \
               $(CP) ../../version_include.man .; \
            fi; \
	    $(DTPLITE) -merge -style ../src/man.css \
                       -header ../src/header.inc \
                       -footer ../src/footer.inc \
                       -o ../html/ html $$srcdir; \
	    $(DTPLITE) -merge -o ../man/ nroff $$srcdir; \
	done
	$(RM) doc/tmp

#
# Testing:
#

NS_TEST_CFG	= -u root -c -d -t $(srcdir)/tests/test.nscfg
NS_TEST_ALL	= $(srcdir)/tests/all.tcl $(TESTFLAGS)
NS_LD_LIBRARY_PATH	= \
   LD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$LD_LIBRARY_PATH" \
   DYLD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$DYLD_LIBRARY_PATH"

EXTRA_TEST_DIRS =
ifneq ($(OPENSSL_LIBS),)
  #EXTRA_TEST_DIRS += nsssl
  PEM_FILE        = tests/testserver/etc/server.pem
  PEM_PRIVATE     = tests/testserver/etc/myprivate.pem
  PEM_PUBLIC      = tests/testserver/etc/mypublic.pem
  SSLCONFIG       = tests/testserver/etc/openssl.cnf
  EXTRA_TEST_REQ  = $(PEM_FILE)
endif

$(PEM_FILE): $(PEM_PRIVATE)
	openssl genrsa 2048 > host.key
	openssl req -new -config $(SSLCONFIG) -x509 -nodes -sha1 -days 365 -key host.key > host.cert
	cat host.cert host.key > server.pem
	rm -rf host.cert host.key
	openssl dhparam 1024 >> server.pem
	mv server.pem $(PEM_FILE)

$(PEM_PRIVATE):
	openssl genrsa -out $(PEM_PRIVATE) 512
	openssl rsa -in $(PEM_PRIVATE) -pubout > $(PEM_PUBLIC)

check: test

test: all $(EXTRA_TEST_REQ)
	$(NS_LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
	@for i in $(EXTRA_TEST_DIRS); do \
		( cd $$i && $(MAKE) test ) || exit 1; \
	done

runtest: all
	$(NS_LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG)

gdbtest: all
	@echo set args $(NS_TEST_CFG) $(NS_TEST_ALL) > gdb.run
	$(NS_LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
	rm gdb.run

lldbtest: all
	$(NS_LD_LIBRARY_PATH) lldb -- ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL) 

lldb-sample: all
	lldb -o run -- $(DESTDIR)$(NAVISERVER)/bin/nsd -f -u nsadmin -t $(DESTDIR)$(NAVISERVER)/conf/nsd-config.tcl


gdbruntest: all
	@echo set args $(NS_TEST_CFG) > gdb.run
	$(NS_LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
	rm gdb.run

memcheck: all
	$(NS_LD_LIBRARY_PATH) valgrind --tool=memcheck ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
helgrind: all
	$(NS_LD_LIBRARY_PATH) valgrind --tool=helgrind ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

cppcheck:
	$(CPPCHECK) --verbose --inconclusive -j4 --enable=all nscp/*.c nscgi/*.c nsd/*.c nsdb/*.c nsproxy/*.c nssock/*.c nsperm/*.c nsssl/*.c \
		-I./include -I/usr/include -D__x86_64__ -DNDEBUG $(DEFS)

CLANG_TIDY_CHECKS=
#CLANG_TIDY_CHECKS=-checks=-*,performance-*,portability-*,cert-*,modernize-*
#CLANG_TIDY_CHECKS=-checks=-*,modernize-*,performance-*,portability-*,cert-*
#CLANG_TIDY_CHECKS=-checks=-*,bugprone-*
clang-tidy:
	clang-tidy-mp-11 nscp/*.c nscgi/*.c nsd/*.c nsdb/*.c nsproxy/*.c nssock/*.c nsperm/*.c \
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
	naviserver-$(NS_PATCH_LEVEL).tar.gz sample-config.tcl

config.guess:
	wget -O config.guess 'https://git.savannah.gnu.org/gitweb/?p=config.git;a=blob_plain;f=config.guess;hb=HEAD'
config.sub:
	wget -O config.sub 'https://git.savannah.gnu.org/gitweb/?p=config.git;a=blob_plain;f=config.sub;hb=HEAD'

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
	tar czf naviserver-$(NS_PATCH_LEVEL).tar.gz --disable-copyfile --exclude="._*" naviserver-$(NS_PATCH_LEVEL)
	$(RM) naviserver-$(NS_PATCH_LEVEL)


.PHONY: all install install-binaries install-doc install-tests clean distclean
