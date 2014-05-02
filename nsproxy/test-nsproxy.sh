#!/bin/sh
#
# test-nsproxy.sh --
#
#   Make sure the nsproxy executable can find the newly built libraries.
#   This wrapper script is specified to be called from: tests/test.nscfg
#

LD_LIBRARY_PATH="../nsd:../nsthread:../nsproxy:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH

exec ../nsproxy/nsproxy $@
