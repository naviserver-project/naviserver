# Root directory, were all huge files are located
set root /tmp/examples/

# Name of current server
set server [ns_info server]

# get query parameter for "file" and "create"
set file [ns_queryget file]
set create [ns_queryget create]

# Create test files
if { $create == 1 } {
    if { [catch {
        file mkdir $root
        set fd [open $root/test1.dat w]
        puts -nonewline $fd [string repeat "test" 1000000]
        close $fd
    } errmsg] } {
        ns_log error "writer.tcl: $errmsg"
    }
}

append data "<h2>Writer Example</h2>"

append data \
    "Writer threads can be used to spool potentially huge files<br>" \
    "to the client using asynchronous I/O. One writer thread can serve<br>" \
    "many clients concurrently with little resource consumptions.<br>" \
    "By using writer threads, connections thread are used for only short<br>" \
    "time periods. Furthermore, this helps against slow read attacks.<p>"

set writerthreads [ns_config ns/server/$server/module/nssock writerthreads]
append data \
    "The current server (named <code>$server</code>) " \
    "is configured with <strong>$writerthreads</strong> writer thread(s).<br>" \
    "You can change the number of writer threads in the used configuration file <br>" \
    "<code>[ns_info config]</code>. <p>The configuration file contains entries like the following:<p>" \
    "<pre>   ns_section ns/server/$server/module/nssock\n   ns_param writerthreads $writerthreads\n" \
    "</pre>"


# Show all files
if { $file eq "" } {

    set files [lsort [glob -nocomplain $root/*.dat]]
    if { [llength $files] == 0 } {
        append data \
            "No test files exist in the configured TMP folder <code>$root</code>.<p>" \
            "Do you want to create them? <a href='?create=1'>Yes</a>"
    } else {
        
        append data "List of available files in <code>$root:</code><ul>"
        foreach file $files {
            set size [file size $file]
            set file [file tail $file]
            append data "<li><a href='?file=$file'>$file</a> - $size bytes</li>"
        }
        append data "</ul>"
    }

    append data \
        "When downloading huge files via writer threads, the size of the NaviSever thread<br>" \
        "will stay small.<p>Back to <a href='.'>example page</a>.<br>"
    
    ns_return 200 text/html $data
    return
}

ns_log notice "check exist [ns_normalizepath $root/$file]"
# Check if it exists
set path [ns_normalizepath $root/$file]
if { ![file exists $path] } {
    ns_returnnotfound
    return
}

# Let the writer to handle headers and size
set rc [ns_writer submitfile -headers $path]
ns_log notice "call submitfile $path -> $rc"
if { $rc } {
    ns_returnnotfound
}
ns_log Notice "file $file has been submitted: $rc"

#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
