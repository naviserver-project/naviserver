<%

    # This is redundant/broken -- should chunk if browser supports
    # chunked encoding and streaming is enabled.
    #

    if {[ns_conn version] == 1.1} {
        ns_conn chunked 1
    }

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