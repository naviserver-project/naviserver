
proc _ns_mktemp_win_temp_dir {} {
    # On Windows create the tempdir for use by ns_mktemp.
    if {! [set win_p [expr {{windows} eq $::tcl_platform(platform)}]]} { return }
    set dd [ns_config ns/parameters tmpdir]
    if {{} eq $dd} { set dd {c:/temp} }
    if {[file isdirectory $dd]} {
        ns_log Debug "Good, tmpdir exists:  $dd"
        return
    }
    if {[catch {file mkdir $dd} errmsg ret_opts]} {
        ns_log Error "Creating tmpdir failed:  $dd  with Error:\n${errmsg}"
    } else {
        ns_log Notice "Successfully created tmpdir:  $dd"
    }
}
