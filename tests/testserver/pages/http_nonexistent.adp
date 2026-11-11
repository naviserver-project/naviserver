<%
    # The caller times out before the local endpoint responds.
    set url [ns_config test listenurl]/adp-test-delayed-response
    ns_adp_puts [ns_http run -timeout 4s $url]
%>
