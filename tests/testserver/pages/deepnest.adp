<%
#
# An ADP page with deeply nested delimiters
#
set choice     [ns_queryget choice 1]

%><%= [ns_adp_parse {<%
	    ns_adp_puts -nonewline [ns_adp_parse {<%
		ns_adp_puts -nonewline "deep"
	    %>}]
	%>}]
%>
