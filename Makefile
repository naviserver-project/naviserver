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

dirs   = nsthread nsd nssock nscgi nscp nslog nsperm nsdb nsdbtest

# Unix only modules
ifeq (,$(findstring MINGW,$(uname)))
   dirs += nsproxy
endif

distfiles = $(dirs) doc tcl contrib include tests win win32 configure m4 \
	Makefile autogen.sh install-sh missing aclocal.m4 configure.ac \
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
	@echo '  install      - install program and man pages to PREFIX ($(PREFIX))'
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
	@echo 'Example for running a single test in the test suite, under the debugger:'
	@echo '  make gdbtest TCLTESTARGS="-file tclconnio.test -match tclconnio-1.1"'
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
	  echo "  chown -R nsadmin $(NAVISERVER)/logs"; \
	  echo ""; \
	  user="-u nsadmin"; \
	  chown -R nobody $(NAVISERVER)/logs; \
	fi; \
	echo "You can now run NaviServer by typing the following command: "; \
	echo ""; \
	echo "  $(NAVISERVER)/bin/nsd -f $$user -t $(NAVISERVER)/conf/nsd-config.tcl"; \
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
	$(CP) -r tests $(INSTSRVPAG)

install-doc:
	@$(MKDIR) $(DESTDIR)$(NAVISERVER)/pages/doc
	$(CP) doc/html/* $(DESTDIR)$(NAVISERVER)/pages/doc
	$(CP) contrib/banners/*.png $(DESTDIR)$(NAVISERVER)/pages/doc

install-examples:
	@$(MKDIR) $(DESTDIR)$(NAVISERVER)/pages/examples
	@for i in contrib/examples/*.adp contrib/examples/*.tcl; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/pages/examples/; \
	done

DTPLITE=dtplite

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
NS_TEST_ALL	= $(srcdir)/tests/all.tcl $(TCLTESTARGS)
LD_LIBRARY_PATH	= \
   LD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$LD_LIBRARY_PATH" \
   DYLD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$DYLD_LIBRARY_PATH"

check: test

test: all
	$(LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

runtest: all
	$(LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG)

gdbtest: all
	@echo set args $(NS_TEST_CFG) $(NS_TEST_ALL) > gdb.run
	$(LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
	rm gdb.run

gdbruntest: all
	@echo set args $(NS_TEST_CFG) > gdb.run
	$(LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
	rm gdb.run

memcheck: all
	$(LD_LIBRARY_PATH) valgrind --tool=memcheck ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
helgrind: all
	$(LD_LIBRARY_PATH) valgrind --tool=helgrind ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

cppcheck:
	cppcheck --enable=all nscp/*.c nscgi/*.c nsd/*.c nsdb/*.c nsproxy/*.c nssock/*.c nsperm/*.c \
		-I./include -I/usr/include -D__x86_64__ $(DEFS)

checkexports: all
	@for i in $(dirs); do \
		nm -p $$i/*.so | awk '$$2 ~ /[TDB]/ { print $$3 }' | sort -n | uniq | grep -v '^[Nn]s\|^TclX\|^_'; \
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

dist: clean
	$(RM) naviserver-$(NS_PATCH_LEVEL)
	$(MKDIR) naviserver-$(NS_PATCH_LEVEL)
	$(CP) $(distfiles) naviserver-$(NS_PATCH_LEVEL)
	$(RM) naviserver-$(NS_PATCH_LEVEL)/include/{config.h,Makefile.global,Makefile.module,stamp-h1}
	hg log --style=changelog > naviserver-$(NS_PATCH_LEVEL)/ChangeLog
	find naviserver-$(NS_PATCH_LEVEL) -name '.[a-zA-Z_]*' -exec rm \{} \;
	tar czf naviserver-$(NS_PATCH_LEVEL).tar.gz --disable-copyfile --exclude="._*" naviserver-$(NS_PATCH_LEVEL)
	$(RM) naviserver-$(NS_PATCH_LEVEL)


.PHONY: all install install-binaries install-doc install-tests clean distclean
