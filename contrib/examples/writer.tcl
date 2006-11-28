# Root were all huge files are located
set root [ns_info home]/modules/movies

# Requested file
set file [ns_queryget file]

# Show all files
if { $file == "" } {
  set data ""
  foreach file [lsort [glob -nocomplain $root/*]] {
    set file [file tail $file]
    append data "<A HREF=?file=$file>$file</A><BR>"
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
