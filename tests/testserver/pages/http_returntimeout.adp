<%
    # The test registers a local endpoint that responds after the client timeout.
    set url "[string trimright [ns_config test listenurl] /]/adp-test-delayed-response"
    ns_adp_puts [ns_http run -timeout 0.5s $url]
%>
