<%
#
# An ADP page with nested delimiters
#
set choice     [ns_queryget choice 1]

%><%= [switch -exact -- $choice {
    case 1 {
    	ns_adp_parse {<% # anything %>anything}
    }
    case 2 {
    	ns_adp_parse {<% # anything %>anything}
    }
    default {
    	ns_adp_parse {<% # anything %>anything}
    }
}]
%>
