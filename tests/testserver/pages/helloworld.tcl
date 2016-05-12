#
# A simple Tcl page.
#

set data   [ns_queryget data "Hello World!"]
set repeat [ns_queryget repeat 1]

ns_return 200 text/plain [string repeat $data $repeat]