<%
    # If streaming is enabled but the browser does not support
    # chunked encoding, revert to Connection: close to mark
    # end of content.

    ns_adp_ctl stream [ns_queryget stream 0]


    # Manipulate the ADP buffer size to force streaming behaviour
    # or fully buffered.

    ns_adp_ctl bufsize [ns_queryget bufsize 8192]


    # When streaming the buffer is flushed after each call to append.
    # Otherwise, everything is buffered and chunking is not required
    # as the content length is known.

    ns_adp_append 0123456789
    ns_adp_append 01234
%>