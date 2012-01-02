<%
#
# A simple ADP page.
#

set data     [ns_queryget data "Hello World!"]
set repeat   [ns_queryget repeat 1]

%><%= [string repeat $data $repeat] %>