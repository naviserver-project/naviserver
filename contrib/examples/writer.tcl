# Root were all huge files are located
set root /tmp

# Requested file
set file [ns_queryget file]
set create [ns_queryget create]

# Create test files
if { $create == 1 } {
  if { [catch {
    set fd [open $root/test1.dat w]
    puts $fd [string repeat "test" 1000000]
    close $fd
  } errmsg] } {
    ns_log error writer.tcl: $errmsg
  }
}

set data "<B>Writer Example</B><P>"
append data "Make sure server is configured with writer thread support in nsd.tcl:<P>"
append data "ns_section ns/server/servername/module/nssock<BR>"
append data "ns_param writerthreads 1<P>"


# Show all files
if { $file == "" } {

  set files [lsort [glob -nocomplain $root/*.dat]]

  if { $files == "" } {
    append data "No test files exist in $root.<P>
                 Do you want to create them? <A HREF=?create=1>Yes</A>"
  } else {
    append data "List of available files in $root:<P>"
    foreach file $files {
      set size [file size $file]
      set file [file tail $file]
      append data "<A HREF=?file=$file>$file - $size bytes</A><BR>"
    }
  }
  ns_return 200 text/html $data
  return
}

# Check if it exists
set path [ns_normalizepath $root/$file]
if { ![file exists $path] } {
  ns_returnnotfound
  return
}

# Let the writer to handle headers and size
set rc [ns_writer submitfile -headers $path]
if { $rc } {
  ns_returnnotfound
}
ns_log Notice file $file has been submitted: $rc
