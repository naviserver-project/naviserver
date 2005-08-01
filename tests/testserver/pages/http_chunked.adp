<%
    # If streaming is enabled but the browser does not support
    # chunked encoding, revert to Connection: close to mark
    # end of content.

    if {[ns_queryget stream 0]} {
        ns_adp_stream
    }

    # When streaming the buffer is flushed after each call to append.
    # Otherwise, everything is buffered and chunking is not required
    # as the content length is known.

    ns_adp_append 0123456789
    ns_adp_append 01234
%>