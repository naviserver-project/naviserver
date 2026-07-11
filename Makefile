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
MAN_CSS=man-5.1.css
HEADER_INC=header-5.1.inc

NSBUILD=1

ifneq ($(MAKECMDGOALS),distclean)
include include/Makefile.global
else
-include include/Makefile.global
endif

#
# Fallback in repeated "make distclean", when Makefile.global is gone
#
RM   ?= /bin/rm -f
RMRF ?= /bin/rm -rf

# Subdirectories
SUBDIRS_CORE := nsthread nsd
SUBDIRS_MODS := nssock nscgi nscp nslog nsperm nsdb nsssl quic revproxy nsdbtest
# Unix only modules
ifeq (,$(findstring MINGW,$(uname)))
   SUBDIRS_MODS += nsproxy
endif
SUBDIRS   := $(SUBDIRS_CORE) $(SUBDIRS_MODS)
TESTDIRS  :=

#
# Use -j per default, obey user serial wish or -j setting
#
USER_SET_J   := $(filter -j%,$(MAKEFLAGS))
ifeq ($(SERIAL),1)
  # stay serial
else ifeq ($(strip $(USER_SET_J)$(HAS_JOBSERVER)),)
  # user did not pass -j -> turn it on by default
  MAKEFLAGS += -j
  ifneq ($(filter 3.%,$(MAKE_VERSION)),)
    # GNU make 3.81 (macOS): pass a bare "-j" to sub-makes
    SUBMAKE_J := -j
  else
    # GNU make >=4: numeric -j is enough; jobserver propagates automatically
    MAKEFLAGS += -j
  endif
endif

VERBOSE ?= 0
ifeq ($(VERBOSE),0)
  SUBMAKE_SILENT := -s
else
  SUBMAKE_SILENT :=
endif

distfiles = $(SUBDIRS) doc tcl contrib include tests win win32 configure m4 \
	Makefile autogen.sh install-sh missing aclocal.m4 configure.ac \
	config.guess config.sub ca-bundle.crt \
	index.adp returnnotice.adp \
	README.md NEWS license.terms naviserver.rdf naviserver.rdf.in \
	version_include.man.in install-from-repository.tcl

distconf = conf/sample-config.tcl.in conf/simple-config.tcl \
	conf/nsd-config.tcl conf/nsd-config.d \
	conf/openacs-config.tcl conf/openacs-config.d

# Top-level goals
all:     $(SUBDIRS:%=all-%) configs
clean:   $(SUBDIRS:%=clean-%)
install: $(SUBDIRS:%=install-%)
test:    $(TESTDIRS:%=test-%)

# One recursive call per subdir/goal, delegate test to selected subdirs
all-%:
	@+$(MAKE) $(SUBMAKE_J) $(SUBMAKE_SILENT) --no-print-directory -C $* all
install-%:
	@+$(MAKE) $(SUBMAKE_J) $(SUBMAKE_SILENT) --no-print-directory -C $* install
clean-%:
	@+$(MAKE) $(SUBMAKE_J) $(SUBMAKE_SILENT) --no-print-directory -C $* clean
test-%:
	+$(MAKE) $(SUBMAKE_J) $(SUBMAKE_SILENT) -C $* test

# Subdir dependencies
all-nsd: | all-nsthread

# modules depend on core
$(SUBDIRS_MODS:%=all-%): | all-nsd

# specific extras
all-nsdbtest: | all-nsdb
#quic: | all-nsssl

# Make sure that install-notice is printed as last thing of a "make install"
$(SUBDIRS:%=install-%): | $(SUBDIRS:%=all-%)
install-notice: | $(SUBDIRS:%=install-%)

help:
	@echo 'Commonly used make targets:'
	@echo '  all          - build program and documentation'
	@echo '  install      - install program and man pages under $(NAVISERVER)'
	@echo '  test         - run all tests in the automatic test suite'
	@echo '  gdbtest      - run all tests, under the control of gdb'
	@echo '  lldbtest     - run all tests, under the control of lldb'
	@echo '  runtest      - start the server in interactive command mode'
	@echo '  gdbruntest   - start the server in command mode, under gdb'
	@echo '  memcheck     - run all tests, under the valgrind memory checker'
	@echo '  build-doc    - build the HTML and nroff documentation'
	@echo '  dist         - create a source tarball naviserver-'$(NS_PATCH_LEVEL)'.tar.gz'
	@echo '  clean        - remove files created by other targets'
	@echo
	@echo 'Example for a system-wide installation under /usr/local/ns:'
	@echo '  make all && su -c "make install"'
	@echo
	@echo 'Example for verbose compilation:'
	@echo '  make VERBOSE=1'
	@echo
	@echo 'Example for running selected test in the test suite, under the debugger:'
	@echo '  make gdbtest TESTFLAGS="-verbose start -file cookies.test -match cookie-2.*"'
	@echo

install: all install-dirs install-include install-tcl install-modules install-config-parameters-dict \
	install-config install-certificates install-doc install-examples install-notice

HAVE_NSADMIN := $(shell id -u nsadmin 2> /dev/null)

install-notice: install-certificates
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
		if ! su -s /bin/sh nsadmin -c "test -w $(NAVISERVER)/logs"; then \
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

DIRS = bin lib logs include tcl pages conf \
       certificates invalid-certificates modules modules/tcl cgi-bin

DIR_TARGETS = $(addprefix $(DESTDIR)$(NAVISERVER)/,$(DIRS))

$(DIR_TARGETS):
	$(MKDIR) $@

install-dirs: $(DIR_TARGETS)

install-config: configs $(DESTDIR)$(NAVISERVER)/pages $(DESTDIR)$(NAVISERVER)/conf
	@for i in returnnotice.adp conf/*.tcl ; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/conf/; \
	done
	@for d in $(FRAGDIRS) ; do \
		test -d $$d || continue; \
		$(MKDIR) $(DESTDIR)$(NAVISERVER)/conf/$$(basename $$d); \
		$(INSTALL_DATA) $$d/*.tcl $(DESTDIR)$(NAVISERVER)/conf/$$(basename $$d)/; \
	done
	@for i in index.adp install-from-repository.tcl; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/pages/; \
	done
	$(INSTALL_SH) install-sh $(DESTDIR)$(INSTBIN)/

install-certificates: $(EXTRA_INSTALL_CERT_REQ) ca-bundle.crt $(DESTDIR)$(NAVISERVER)/certificates $(DESTDIR)$(NAVISERVER)/invalid-certificates
	@if [ -d "$(DESTDIR)$(NAVISERVER)/etc" ]; then \
		for i in "$(DESTDIR)$(NAVISERVER)"/etc/*pem; do \
			if [ -f "$$i" ]; then \
				$(LN) -sf "$$i" "$(DESTDIR)$(NAVISERVER)/certificates/"; \
			fi; \
		done; \
	fi
	@for i in ./certificates/*; do \
		if [ -f "$$i" ]; then \
			case "$$i" in \
				*.key|*.csr|*.srl|*.ext) ;; \
				*) $(INSTALL_DATA) "$$i" "$(DESTDIR)$(NAVISERVER)/certificates/" ;; \
			esac; \
		fi; \
	done
	@if [ -n "$(OPENSSL_LIBS)" ]; then \
	        $(OPENSSL) rehash "$(DESTDIR)$(NAVISERVER)/certificates" 2>/dev/null || true; \
	fi
	@$(INSTALL_DATA) ca-bundle.crt "$(DESTDIR)$(NAVISERVER)/"

install-modules: $(DESTDIR)$(NAVISERVER)/modules $(DESTDIR)$(NAVISERVER)/modules/tcl
	@for i in $(dirs); do \
		(cd $$i && $(MAKE) install) || exit 1; \
	done

install-tcl: $(DESTDIR)$(NAVISERVER)/tcl
	@for i in tcl/*.tcl; do \
		$(INSTALL_DATA) -t $(DESTDIR)$(NAVISERVER)/tcl/ $$i; \
	done

install-include: $(DESTDIR)$(NAVISERVER)/include
	@for i in include/*.h include/Makefile.global include/Makefile.module; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(INSTHDR)/; \
	done

install-tests: install-dirs
	$(CP) tests $(INSTSRVPAG)

install-config-parameters-dict: $(DESTDIR)$(NAVISERVER)/modules/tcl
	@$(INSTALL_DATA) $(CONFIG_PARAMETERS_DICT) \
	    $(DESTDIR)$(NAVISERVER)/modules/tcl/$(notdir $(CONFIG_PARAMETERS_DICT))

install-doc: $(DESTDIR)$(NAVISERVER)/pages
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

install-examples: $(DESTDIR)$(NAVISERVER)/pages
	@$(MKDIR) $(DESTDIR)$(NAVISERVER)/pages/examples
	@for i in contrib/examples/*.adp contrib/examples/*.tcl; do \
		$(INSTALL_DATA) $$i $(DESTDIR)$(NAVISERVER)/pages/examples/; \
	done


CONFIG_PARAMETERS_DICT ?= doc/tcl/config-parameters.tcl
CONFIG_PARAMETERS_GEN   = doc/tools/gen-config-parameters.tcl
CONFIG_PARAMETERS_DIR   = doc/src/include

CONFIG_PARAMETERS_INCLUDES = \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--parameters.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--parameters--reverseproxymode.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--threads.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--mimetypes.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--fastpath.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--reverseproxymode.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--modules.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--servers.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nssock.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nsssl.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-quic.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--pools.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--pool--star.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--adp.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--fastpath.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--httpclient.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--tcl.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--vhost.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nslog.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nsperm.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nsproxy.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nscgi.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nscp.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-revproxy.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-revproxy-backends.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nsdb.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--server--star--db.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--db--pools.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--db--pool--star.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--db--drivers.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--db--driver--star.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--db--driver--postgres.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--db--driver--nsoracle.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nsstats.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-nssmtpd.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-ns--sendmail.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-nscgi.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-nscp.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-nsperm.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-nsproxy.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-revproxy.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-revproxy-backends.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-nsdb.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-params-ns--server--star--adp.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-params-ns--server--star--httpclient.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-params-ns--fastpath.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-params-ns--server--star--fastpath.man \
	$(CONFIG_PARAMETERS_DIR)/config-parameters-params-ns--sendmail.man


$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-%.man: $(CONFIG_PARAMETERS_DICT)
	@mkdir -p $(dir $@)
	$(TCLSH_PROG) $(CONFIG_PARAMETERS_GEN) \
	    --config $(CONFIG_PARAMETERS_DICT) \
	    --module "$*" \
	    --parameters-only \
	    --output $@
$(CONFIG_PARAMETERS_DIR)/config-parameters-module-%.man: $(CONFIG_PARAMETERS_DICT)
	@mkdir -p $(dir $@)
	$(TCLSH_PROG) $(CONFIG_PARAMETERS_GEN) \
	    --config $(CONFIG_PARAMETERS_DICT) \
	    --module "$*" \
	    --title "$*" \
	    --output $@
$(CONFIG_PARAMETERS_DIR)/config-parameters-params-%.man: $(CONFIG_PARAMETERS_DICT)
	@mkdir -p $(dir $@)
	$(TCLSH_PROG) $(CONFIG_PARAMETERS_GEN) \
	    --config $(CONFIG_PARAMETERS_DICT) \
	    --section "$(subst star,*,$(subst --,/,$*))" \
	    --parameters-only \
	    --output $@
$(CONFIG_PARAMETERS_DIR)/config-parameters-%.man: $(CONFIG_PARAMETERS_DICT)
	@mkdir -p $(dir $@)
	$(TCLSH_PROG) $(CONFIG_PARAMETERS_GEN) \
	    --config $(CONFIG_PARAMETERS_DICT) \
	    --section "$(subst star,*,$(subst --,/,$*))" \
	    --output $@
$(CONFIG_PARAMETERS_DIR)/config-parameters-module-revproxy-backends.man: $(CONFIG_PARAMETERS_DICT)
	@mkdir -p $(dir $@)
	$(TCLSH_PROG) $(CONFIG_PARAMETERS_GEN) \
	    --config $(CONFIG_PARAMETERS_DICT) \
	    --section "ns/server/*/module/revproxy/*" \
	    --output $@
$(CONFIG_PARAMETERS_DIR)/config-parameters-module-params-revproxy-backends.man: $(CONFIG_PARAMETERS_DICT)
	@mkdir -p $(dir $@)
	$(TCLSH_PROG) $(CONFIG_PARAMETERS_GEN) \
	    --config $(CONFIG_PARAMETERS_DICT) \
	    --parameters-only \
	    --section "ns/server/*/module/revproxy/*" \
	    --output $@

REGEN_CONFIG_PARAMETERS ?= 0

ifeq ($(REGEN_CONFIG_PARAMETERS),1)
config-parameters-includes: $(CONFIG_PARAMETERS_INCLUDES)
else
config-parameters-includes:
	@missing=0; \
	for f in $(CONFIG_PARAMETERS_INCLUDES); do \
	    if test ! -f "$$f"; then \
	        echo "missing generated config parameter include: $$f"; \
	        missing=1; \
	    fi; \
	done; \
	if test "$$missing" != 0; then \
	    echo "run 'make config-parameters-includes REGEN_CONFIG_PARAMETERS=1' in an environment with $(TCLSH_PROG)"; \
	    exit 1; \
	fi

endif


regen-config-parameters-includes:
	$(MAKE) config-parameters-includes REGEN_CONFIG_PARAMETERS=1

# On some systems you may need a special shell script to control the
# PATH seen by dtplite, as that influences which versions of $(TCLSH_PROG) and
# Tcllib dtplite uses.  I personally found it necessary to use a
# one-line script like this for the DTPLITE command:
#   env PATH=/usr/sbin:/usr/bin:/sbin:/bin dtplite "$@"
# --atp@piskorski.com, 2014/08/27 10:52 EDT

ifeq ($(DTPLITE),)
  DTPLITE=dtplite
else
  # Do nothing, use the environment variable as is.
endif

build-doc: config-parameters-includes
	$(RMRF) doc/html doc/man doc/tmp
	$(MKDIR) doc/html doc/man doc/tmp
	@for srcdir in nscgi \
		       nslog \
		       nsdb \
		       nsproxy \
		       nscp \
		       nsperm \
		       nssock \
		       nsssl \
		       quic \
		       revproxy \
		       doc/src/manual \
		       doc/src/naviserver \
		       doc/src/include \
		       modules/nsexpat \
		       modules/nsconfigrw \
		       modules/nsdbi \
		       modules/nsloopctl \
		       modules/nsvfs; do \
		if [ -d $$srcdir ]; then \
		   echo "cp $$srcdir/*.man to doc/tmp/`basename $$srcdir`"; \
		   $(MKDIR) doc/tmp/`basename $$srcdir`; \
		   find $$srcdir -name '*.man' -exec $(CP) "{}" doc/tmp/`basename $$srcdir` ";"; \
		fi; \
	done
	find doc/tmp -name '*.man'
	$(CP) doc/images/manual/*.png doc/tmp/manual/
	$(CP) doc/images/naviserver/*.png doc/tmp/naviserver/
	$(CP) revproxy/doc/mann/*.png doc/tmp/revproxy/
	$(CP) doc/commandlist_include.man doc/tmp/
	@cd doc/tmp; \
	for srcdir in */; do \
	  srcdir=$${srcdir%/}; \
	  case "$$srcdir" in \
	    include) continue ;; \
	  esac; \
          echo "$$srcdir"; \
	  if [ -f "$$srcdir/version_include.man" ]; then \
	    $(CP) "$$srcdir/version_include.man" .; \
	  else \
	    $(CP) ../../version_include.man .; \
	  fi; \
	  if [ "$(VALIDATE_DOC)" = "1" ]; then \
	    for f in $$(find "$$srcdir" -type f  -name '*.man' ! -name '.*' ! -name '*_include.man' | sort); do \
              echo "validating $$f"; \
              $(DTPLITE) validate "$$f" || { \
                echo "ERROR: dtplite validate failed for $$f"; \
                exit 1; \
              }; \
            done; \
          fi; \
	  echo $(DTPLITE) -merge -style ../src/$(MAN_CSS) \
		       -header ../src/$(HEADER_INC) \
		       -footer ../src/footer.inc \
		       -o ../html/ html "$$srcdir"; \
	  $(DTPLITE) -merge -style ../src/$(MAN_CSS) \
		       -header ../src/$(HEADER_INC) \
		       -footer ../src/footer.inc \
		       -o ../html/ html "$$srcdir" || { \
                    echo "ERROR: dtplite HTML merge failed for $$srcdir"; \
                    exit 1; \
               }; \
	  $(DTPLITE) -merge -o ../man/ nroff "$$srcdir" || { \
                    echo "ERROR: dtplite nroff merge failed for $$srcdir"; \
                    exit 1; \
               }; \
	done
	$(RMRF) doc/tmp

#
# Local copy of Source Forge Documentation
#
SF_HTDOCS         ?= /usr/local/ns/sf-naviserver-htdocs
DOC_VERSION_FILE  ?= ./version_include.man
DOC_VERSION       := $(shell sed -n 's/^\[vset version \([^]]*\)\]/\1/p' $(DOC_VERSION_FILE))
DOC_VERSION_MINOR := $(shell printf '%s\n' '$(DOC_VERSION)' | sed 's/^\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/')

install-sf-doc:
	@if [ -z "$(DOC_VERSION)" ] || [ -z "$(DOC_VERSION_MINOR)" ]; then \
		echo "Cannot determine documentation version from $(DOC_VERSION_FILE)" ; \
		exit 1 ; \
	fi
	@if [ ! -d doc/html ]; then \
		echo "No generated documentation found in doc/html." ; \
		echo "Run: make build-doc" ; \
		exit 1 ; \
	fi
	@target="$(SF_HTDOCS)/$(DOC_VERSION_MINOR)" ; \
	tmp="$${target}.new" ; \
	old="$${target}.old" ; \
	echo "Installing documentation version $(DOC_VERSION) as $(DOC_VERSION_MINOR)" ; \
	echo "Target: $${target}" ; \
	$(RM) -rf "$${tmp}" ; \
	$(MKDIR) "$${tmp}" ; \
	$(CP) doc/html/* "$${tmp}/" ; \
	find "$${tmp}" -name '*-original' -type f -exec rm -f {} + ; \
	$(MKDIR) "$${tmp}/naviserver/files" ; \
	$(CP) "doc/src/$(MAN_CSS)" "$${tmp}/naviserver/" ; \
	$(CP) "$${tmp}/naviserver/$(MAN_CSS)" "$${tmp}/naviserver/files/" ; \
	( cd "$${tmp}/naviserver" || exit 1 ; $(RM) -f index.html ; $(LN) -s toc.html index.html ) ; \
	$(RM) -rf "$${old}" ; \
	if [ -d "$${target}" ] || [ -L "$${target}" ]; then mv "$${target}" "$${old}" ; fi ; \
	mv "$${tmp}" "$${target}" ; \
	echo "" ; \
	echo "Installed SourceForge documentation tree:" ; \
	echo "  $${target}" ; \
	echo "" ; \
	if [ -d "$${old}" ]; then \
		echo "Previous tree saved as:" ; \
		echo "  $${old}" ; \
		echo "" ; \
		echo "Review changes with:" ; \
		echo "  diff -rwu $${old}/ $${target}/" ; \
		echo "" ; \
	fi ; \
	echo "Inspect the documentation tree with:" ; \
	echo "  (cd $(SF_HTDOCS); tree -a -F -L 2)" ; \
	echo "" ; \
	echo "Preview locally with:" ; \
	echo "  nsd_pagedir=$(SF_HTDOCS) /usr/local/ns/bin/nsd -t /usr/local/ns/conf/nsd-config.tcl -f"

#
# Testing:
#
NS_TEST_ENV     = LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8

ifneq ($(strip $(DYLD_INSERT_LIBRARIES)),)
NS_TEST_ENV += DYLD_INSERT_LIBRARIES="$(DYLD_INSERT_LIBRARIES)"
endif
ifneq ($(strip $(LD_PRELOAD)),)
NS_TEST_ENV += DYLD_INSERT_LIBRARIES="$(LD_PRELOAD)"
endif

ifeq ($(shell id -u),0)
NS_TEST_CFG	= -u nsadmin -c -d -t $(srcdir)/tests/test.nscfg
else
NS_TEST_CFG	= -c -d -t $(srcdir)/tests/test.nscfg
endif


NS_TEST_ALL	= $(srcdir)/tests/all.tcl $(TESTFLAGS)
NS_LD_LIBRARY_PATH	= \
   LD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$LD_LIBRARY_PATH" \
   DYLD_LIBRARY_PATH="$(srcdir)/nsd:$(srcdir)/nsthread:$(srcdir)/nsdb:$(srcdir)/nsproxy:$$DYLD_LIBRARY_PATH"

#
# Handling installs without OpenSSL
#
ifneq ($(OPENSSL_LIBS),)
  EXTRA_TEST_REQ += test-certificates
  EXTRA_INSTALL_CERT_REQ = demo-certificates
else
  EXTRA_INSTALL_CERT_REQ =
demo-certificates:
	@echo "OpenSSL support not configured; skipping demo certificate generation"

test-certificates:
	@echo "OpenSSL support not configured; skipping test certificate generation"
endif

# --------------------------------------------------------------------
# Demo Certificates
# --------------------------------------------------------------------
CERT_DIR = certificates

CA_CSR      = $(CERT_DIR)/ca.csr
CA_EXT      = $(CERT_DIR)/ca.ext
CA_KEY      = $(CERT_DIR)/ca.key
CA_CERT     = $(CERT_DIR)/ca.crt

SERVER_EXT  = $(CERT_DIR)/server.ext
SERVER_KEY  = $(CERT_DIR)/server.key
SERVER_PUB  = $(CERT_DIR)/server-public.pem
SERVER_CSR  = $(CERT_DIR)/server.csr
SERVER_CERT = $(CERT_DIR)/server.crt
SERVER_PEM  = $(CERT_DIR)/server.pem

CLIENT_EXT  = $(CERT_DIR)/client.ext
CLIENT_KEY  = $(CERT_DIR)/client.key
CLIENT_CSR  = $(CERT_DIR)/client.csr
CLIENT_CERT = $(CERT_DIR)/client.crt

$(CERT_DIR):
	$(MKDIR) $(CERT_DIR)

$(CA_CSR): $(CA_KEY)
	$(OPENSSL) req -new \
	    -key $(CA_KEY) \
	    -out $@ \
	    -subj "/CN=NaviServer Demo CA"

$(CA_EXT): | $(CERT_DIR)
	@printf '%s\n' \
	    '[ca_ext]' \
	    'basicConstraints = critical, CA:TRUE' \
	    'keyUsage = critical, keyCertSign, cRLSign' \
	    'subjectKeyIdentifier = hash' \
	    > $@

$(CA_KEY): | $(CERT_DIR)
	$(OPENSSL) genrsa -out $@ 4096

$(CA_CERT): $(CA_CSR) $(CA_KEY) $(CA_EXT)
	$(OPENSSL) x509 -req \
	    -in $(CA_CSR) \
	    -signkey $(CA_KEY) \
	    -out $@ \
	    -days 3650 -sha256 \
	    -extfile $(CA_EXT) -extensions ca_ext

$(SERVER_EXT): | $(CERT_DIR)
	@printf '%s\n' \
	    '[server_ext]' \
	    'basicConstraints = CA:FALSE' \
	    'keyUsage = digitalSignature, keyEncipherment' \
	    'extendedKeyUsage = serverAuth' \
	    'subjectAltName = @alt_names' \
	    '' \
	    '[alt_names]' \
	    'DNS.1 = localhost' \
	    'IP.1 = 127.0.0.1' \
	    'IP.2 = ::1' \
	    > $@

$(SERVER_KEY): | $(CERT_DIR)
	$(OPENSSL) genrsa -out $@ 2048

$(SERVER_PUB): $(SERVER_KEY)
	$(OPENSSL) rsa -in $(SERVER_KEY) -pubout -out $@

$(SERVER_CSR): $(SERVER_KEY)
	$(OPENSSL) req -new -key $< -out $@ -subj "/CN=localhost"

$(SERVER_CERT): $(SERVER_CSR) $(CA_CERT) $(CA_KEY) $(SERVER_EXT)
	$(OPENSSL) x509 -req -in $(SERVER_CSR) \
	    -CA $(CA_CERT) -CAkey $(CA_KEY) \
	    -CAserial $(CERT_DIR)/server.srl -CAcreateserial \
	    -out $@ -days 365 -sha256 \
	    -extfile $(SERVER_EXT) -extensions server_ext

$(SERVER_PEM): $(SERVER_CERT) $(SERVER_KEY)
	cat $(SERVER_CERT) $(SERVER_KEY) > $@


$(CLIENT_EXT): | $(CERT_DIR)
	@printf '%s\n' \
	    '[client_ext]' \
	    'basicConstraints = CA:FALSE' \
	    'keyUsage = digitalSignature' \
	    'extendedKeyUsage = clientAuth' \
	    'subjectAltName = @alt_names' \
	    '' \
	    '[alt_names]' \
	    'DNS.1 = client1.example.org' \
	    'DNS.2 = client2.example.org' \
	    'email.1 = test-client@example.org' \
	    'URI.1 = spiffe://example.org/user/test-client' \
	    > $@

$(CLIENT_KEY): | $(CERT_DIR)
	$(OPENSSL) genrsa -out $@ 2048

$(CLIENT_CSR): $(CLIENT_KEY)
	$(OPENSSL) req -new -key $< -out $@ -subj "/CN=test-client"

$(CLIENT_CERT): $(CLIENT_CSR) $(CA_CERT) $(CA_KEY) $(CLIENT_EXT)
	$(OPENSSL) x509 -req -in $(CLIENT_CSR) \
	    -CA $(CA_CERT) -CAkey $(CA_KEY) \
	    -CAserial $(CERT_DIR)/client.srl -CAcreateserial \
	    -out $@ -days 365 -sha256 \
	    -extfile $(CLIENT_EXT) -extensions client_ext

ca-bundle.crt:
	curl -s -fS -k -L -o ca-bundle.crt 'https://raw.githubusercontent.com/bagder/ca-bundle/refs/heads/master/ca-bundle.crt'

demo-certificates: $(SERVER_PEM) $(CLIENT_CERT) $(SERVER_PUB)
	@($(OPENSSL) rehash $(CERT_DIR) 2>/dev/null || true)

# --------------------------------------------------------------------
# Test Certificates
# --------------------------------------------------------------------
TEST_CERT_DIR = tests/testserver/certificates

$(TEST_CERT_DIR):
	$(MKDIR) $(TEST_CERT_DIR)

test-certificates: demo-certificates $(TEST_CERT_DIR)
	@for i in ca.crt server.crt server.key server-public.pem server.pem client.crt client.key client.ext server.ext ca.ext; do \
		$(CP) certificates/$$i $(TEST_CERT_DIR)/; \
	done
	@if [ -n "$(OPENSSL_LIBS)" ]; then \
		$(OPENSSL) rehash "$(TEST_CERT_DIR)" 2>/dev/null || true; \
	fi

DISTCLEAN_CERTS = \
	$(CA_KEY) $(CA_CERT) $(CA_CSR) $(CERT_DIR)/ca.ext $(CERT_DIR)/ca.srl \
	$(SERVER_KEY) $(SERVER_PUB) $(SERVER_CSR) $(SERVER_CERT) $(SERVER_PEM) $(CERT_DIR)/server.ext $(CERT_DIR)/server.srl \
	$(CLIENT_KEY) $(CLIENT_CSR) $(CLIENT_CERT) $(CERT_DIR)/client.ext $(CERT_DIR)/client.srl \
	$(TEST_CERT_DIR)/server.pem $(TEST_CERT_DIR)/server.key $(TEST_CERT_DIR)/server-public.pem $(TEST_CERT_DIR)/server.csr \
	$(TEST_CERT_DIR)/server.crt $(TEST_CERT_DIR)/server.ext $(TEST_CERT_DIR)/server.srl \
	$(TEST_CERT_DIR)/client.key $(TEST_CERT_DIR)/client.csr $(TEST_CERT_DIR)/client.crt $(TEST_CERT_DIR)/client.srl \
	$(TEST_CERT_DIR)/client.ext $(TEST_CERT_DIR)/ca.key \
	$(TEST_CERT_DIR)/ca.crt $(TEST_CERT_DIR)/ca.csr $(TEST_CERT_DIR)/ca.ext $(TEST_CERT_DIR)/ca.srl

# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------

check: test

test: all $(EXTRA_TEST_REQ)
	@if [ $(shell id -u) = 0 ]; then \
	    $(CHOWN) -R nsadmin $(srcdir)/tests ; \
	fi;
	$(NS_TEST_ENV) $(NS_LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

runtest: all
	$(NS_TEST_ENV) $(NS_LD_LIBRARY_PATH) ./nsd/nsd $(NS_TEST_CFG)

gdbtest: all
	$(NS_TEST_ENV) $(NS_LD_LIBRARY_PATH) gdb -ex=run --args ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
#	@echo set args $(NS_TEST_CFG) $(NS_TEST_ALL) > gdb.run
#	$(NS_LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
#	$(RM) gdb.run

lldbtest: all
	$(NS_TEST_ENV) $(NS_LD_LIBRARY_PATH) lldb -- ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

#lldb-sample: all
#	lldb -o run -- $(DESTDIR)$(NAVISERVER)/bin/nsd -f -u nsadmin -t $(DESTDIR)$(NAVISERVER)/conf/nsd-config.tcl


gdbruntest: all
	@echo set args $(NS_TEST_CFG) > gdb.run
	$(NS_TEST_ENV) $(NS_LD_LIBRARY_PATH) gdb -x gdb.run ./nsd/nsd
	$(RM) gdb.run

memcheck: all
	$(NS_TEST_ENV) $(NS_LD_LIBRARY_PATH) valgrind --tool=memcheck ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)
helgrind: all
	$(NS_TEST_ENV) $(NS_LD_LIBRARY_PATH) valgrind --tool=helgrind ./nsd/nsd $(NS_TEST_CFG) $(NS_TEST_ALL)

CPPCHECK_SYS_INCLUDES=-I/usr/include
#CPPCHECK_SYS_INCLUDES=-I`xcrun --show-sdk-path`/usr/include

cppcheck:
	$(CPPCHECK) --verbose --inconclusive -j4 --enable=all --check-level=exhaustive --suppress=missingIncludeSystem \
		--output-file=cppcheck-output.txt --checkers-report=cppcheck.txt  \
		nscp/*.c nscgi/*.c nsd/*.c nsdb/*.c nsproxy/*.c nssock/*.c nsperm/*.c nsssl/*.c quic/*.c \
		-I./include  -I./nsssl -I./quic $(CPPCHECK_SYS_INCLUDES) -D__x86_64__ -DNDEBUG $(DEFS)
	echo "log written to cppcheck-output.txt"

CLANG_TIDY_CHECKS=
#CLANG_TIDY_CHECKS=-*,performance-*,portability-*,cert-*,modernize-*
#CLANG_TIDY_CHECKS=-*,clang-analyzer-core.*,clang-analyzer-unix.*
#CLANG_TIDY_CHECKS=-*,clang-analyzer-security.*,security-*
#CLANG_TIDY_CHECKS=-*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-reserved-identifier
#CLANG_TIDY_CHECKS=-*,cert-*,-cert-err33-c,-cert-dcl37-c,-cert-dcl51-cpp
#CLANG_TIDY_CHECKS=-*,performance-*,portability-*,cert-*,modernize-*
#CLANG_TIDY_CHECKS=-*,clang-analyzer-core.*,clang-analyzer-unix.*,clang-analyzer-security.*,bugprone-*,cert-*,-performance-no-int-to-ptr,-modernize-*,-cert-dcl37-c,-cert-dcl51-cpp,-cert-err33-c,-bugprone-multi-level-implicit-pointer-conversion,-bugprone-casting-through-void,-bugprone-macro-parentheses,-bugprone-easily-swappable-parameters

clang-tidy:
	clang-tidy-mp-22 nscp/*.c nscgi/*.c nsd/*.c nsdb/*.c nsproxy/*.c nssock/*.c nsperm/*.c \
               -checks=-*,$(CLANG_TIDY_CHECKS) -export-fixes=clang-tidy-fixes.yaml -- \
               -I./include -I/usr/include $(DEFS) \
               2>&1 | tee clang-tidy-output.txt



# --------------------------------------------------------------------
# Misc
# --------------------------------------------------------------------


checkexports: all
	@for i in $(SUBDIRS); do \
		nm -p $$i/*${LIBEXT} | awk '$$2 ~ /[TDB]/ { print $$3 }' | sort -n | uniq | grep -v '^[Nn]s\|^TclX\|^_'; \
	done

clean-bak: clean
	@find . -name '*~' -exec rm "{}" ";"

distclean: clean
	$(RMRF) autom4te.cache
	$(RM) config.status config.log config.cache aclocal.m4 configure \
		include/{Makefile.global,Makefile.module,config.h,config.h.in,stamp-h1} \
		naviserver-$(NS_PATCH_LEVEL).tar.gz sample-config.tcl \
		$(DISTCLEAN_CERTS)
	@find "$(CERT_DIR)" "$(TEST_CERT_DIR)" -type l -delete 2>/dev/null || true

config.guess:
	curl -s -fS -k -L -o config.guess 'https://git.savannah.gnu.org/gitweb/?p=config.git;a=blob_plain;f=config.guess;hb=HEAD'
config.sub:
	curl -s -fS -k -L -o config.sub 'https://git.savannah.gnu.org/gitweb/?p=config.git;a=blob_plain;f=config.sub;hb=HEAD'

dist: config.guess config.sub clean configs
	$(RMRF) naviserver-$(NS_PATCH_LEVEL)
	$(MKDIR) naviserver-$(NS_PATCH_LEVEL)
	$(MKDIR) naviserver-$(NS_PATCH_LEVEL)/conf
	$(MKDIR) naviserver-$(NS_PATCH_LEVEL)/certificates
	$(CP) $(distfiles) naviserver-$(NS_PATCH_LEVEL)
	$(CP) $(distconf) naviserver-$(NS_PATCH_LEVEL)/conf/
	$(RM) naviserver-$(NS_PATCH_LEVEL)/include/{config.h,Makefile.global,Makefile.module,stamp-h1}
	$(RM) naviserver-$(NS_PATCH_LEVEL)/tcl/initdebug.tcl
	$(RM) naviserver-$(NS_PATCH_LEVEL)/*/*-{debug,gn} naviserver-$(NS_PATCH_LEVEL)/*/*/*-{debug,gn}
	$(RM) naviserver-$(NS_PATCH_LEVEL)/*/*.pid naviserver-$(NS_PATCH_LEVEL)/*/*/*.pid  naviserver-$(NS_PATCH_LEVEL)/*/*/*/*.pid
	$(RM) naviserver-$(NS_PATCH_LEVEL)/*/*.diff
	$(RM) naviserver-$(NS_PATCH_LEVEL)/tests/testserver/access.log
	git log --date-order --name-status --date=short  >naviserver-$(NS_PATCH_LEVEL)/ChangeLog
	if [ -f $(HOME)/scripts/fix-typos.tcl ]; then \
		(cd naviserver-$(NS_PATCH_LEVEL)/; $(TCLSH_PROG) $(HOME)/scripts/fix-typos.tcl -name Change\*) \
	fi;
	find naviserver-$(NS_PATCH_LEVEL) -type f -name '.[a-zA-Z_]*' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*-original' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '[a-z]*.pem' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*.c-*' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*.h-*' -exec rm \{} \;
	find naviserver-$(NS_PATCH_LEVEL) -name '*~' -exec rm \{} \;
	tar czf naviserver-$(NS_PATCH_LEVEL).tar.gz --exclude='*/.*' --no-xattrs --disable-copyfile --exclude="._*" naviserver-$(NS_PATCH_LEVEL)
	$(RMRF) naviserver-$(NS_PATCH_LEVEL)

# --------------------------------------------------------------------
# Config generation from fragment directories
# --------------------------------------------------------------------

FRAGDIRS := conf/openacs-config.d conf/nsd-config.d
# Map "conf/foo.d" -> "foo.tcl" (generated at repo top-level)
#GENCONFIGS := $(patsubst conf/%.d,%.tcl,$(FRAGDIRS))

# Map "conf/foo-config.d" -> "conf/foo-config.tcl"
CONFIGS  := $(patsubst %.d,%.tcl,$(FRAGDIRS))

# Helper: list of fragments for a given dir, sorted lexicographically
frags = $(sort $(wildcard $(1)/*.tcl))

# Create a concatenated file from a directory
define GEN_CONFIG_template
$(patsubst %.d,%.tcl,$(1)): $$(call frags,$(1))
	@echo "generate  $$@"
	@if test -z "$$(strip $$(call frags,$(1)))"; then \
	  echo "ERROR: no fragments found in $(1)/*.tcl" 1>&2; \
	  exit 1; \
	fi
	@{ \
	  echo "########################################################################"; \
	  echo "# GENERATED FILE -- do not edit"; \
	  echo "# Source: $(1)/"; \
	  echo "########################################################################"; \
	  echo; \
	  $(foreach f,$(call frags,$(1)), \
	    echo "# source: $(patsubst conf/%,%,$(f))"; \
	    cat "$(f)"; \
	    echo; \
	  ) \
	} > $$@
endef

$(foreach d,$(FRAGDIRS),$(eval $(call GEN_CONFIG_template,$(d))))

configs: $(CONFIGS)


.PHONY: all install clean distclean \
	install-dirs install-include install-tcl install-modules install-config-parameters-dict config-parameters-includes configs \
	install-config install-certificates install-doc install-examples install-notice \
	all-% install-% clean-% test-%

#.NOTPARALLEL: install install-tcl install-dirs
