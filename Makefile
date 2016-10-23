#
# Support for multiple NaviServer installations on a single host
#
ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Name of the modules
#
MODNAME = revproxy

#
# List of components to be installed as the the Tcl module section
#
TCL =	revproxy-procs.tcl \
	README

#
# Get the common Makefile rules
#
include  $(NAVISERVER)/include/Makefile.module

