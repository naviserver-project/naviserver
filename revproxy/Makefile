#
# Name of the modules
#
MODNAME = revproxy

# Keep the release version in revproxy-procs.tcl; the build tag is NaviServer's.
NS_MODULE_VERSION := $(shell sed -n 's/^    set version \(.*\)$$/\1/p' revproxy-procs.tcl)
TCL_INIT = init.tcl.in

#
# List of components to be installed as the the Tcl module section
#
TCL =	revproxy-procs.tcl \
	revproxy-ns-http-procs.tcl \
	revproxy-ns-connchan-procs.tcl

#
# Get the common Makefile rules
#
include ../include/Makefile.build
