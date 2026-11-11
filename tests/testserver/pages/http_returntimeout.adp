<%
    # The upstream request times out before the local endpoint responds.
    set url [ns_config test listenurl]/adp-test-delayed-response
    ns_adp_puts [ns_http run -timeout 0.5s $url]
%>
