<%
    # http request to a nonexistent address
    ns_adp_puts [ns_http run -timeout 0.5s http://192.0.2.1/]
%>
