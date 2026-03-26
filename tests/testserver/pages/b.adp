begin of b.adp<br> ...<br>
<% ns_adp_puts "Hello, world!" %><br>
<% ns_adp_puts "ns_adp_argc [ns_adp_argc]" %><br>
<% ns_adp_puts "ns_adp_argv [ns_adp_argv]" %><br>
<%
  ns_adp_bind_args v1 v2 v3 v4 v5 v6 v7
  ns_adp_puts "v1..v7: $v1, $v2, $v3, $v4, $v5, $v6, $v7" ;# => {1 2 3 a A z Z }
%> <br>
...<br> end of b.adp<br>