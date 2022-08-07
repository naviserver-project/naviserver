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

MODNAME  =  nsperm
MOD      =  nsperm.so
MODOBJS  =  nsperm.o
TCL      =  init.tcl compat.tcl htaccess.tcl
MODINST	 =  cpfiles

include ../include/Makefile.build

moddat	 = $(DESTDIR)$(INSTSRVMOD)/$(MODNAME)

cpfiles:
	@$(MKDIR) $(moddat)
	for f in passwd group hosts.allow hosts.deny perms; do \
		test -f $(moddat)/$$f || $(INSTALL_DATA) $$f $(moddat)/; \
	done
