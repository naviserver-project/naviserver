# Run tests which must emit diagnostics in a captured child during quiet runs.
# Preserve real assertions, test selection, counts and complete failure output.
# A -verbose start run executes directly so diagnostics remain visible.
proc ::tcltest::runQuietLoggingTests {testFile argv} {
    if {"start" in [::tcltest::configure -verbose]} {return 0}
    set childName [file rootname [file tail $testFile]]-quiet-child.nscfg
    set childConfig [::tcltest::makeFile {} $childName]
    set configChannel [open $childConfig w]
    puts $configChannel [list source [file join [ns_config test home] test.nscfg]]
    puts $configChannel [list ns_section -update ns/parameters]
    puts $configChannel [list ns_param pidfile $childConfig.pid]
    close $configChannel
    set parentDirectory [pwd]
    set childCode [catch {
        cd [file dirname [ns_config test home]]
        exec -- [ns_info nsd] -c -d -t $childConfig \
            [file join [ns_config test home] all.tcl] \
            {*}$argv -file [file tail $testFile] -verbose {start error} 2>@1
    } childOutput childOptions]
    cd $parentDirectory
    ::tcltest::removeFile $childName
    if {![regexp {all[.]tcl:[ \t]+Total[ \t]+([0-9]+)[ \t]+Passed[ \t]+([0-9]+)[ \t]+Skipped[ \t]+([0-9]+)[ \t]+Failed[ \t]+([0-9]+)} \
              $childOutput -> total passed skipped failed]} {
        error "logging regression child did not report test results:\n$childOutput"
    }
    if {$childCode && $failed == 0} {
        # Some command-mode builds finish a successful ns_shutdown via SIGTERM.
        set ec [dict get $childOptions -errorcode]
        if {[lindex $ec 0] ne "CHILDKILLED" || [lindex $ec 2] ne "SIGTERM"} {
            error "logging regression child failed after reporting results:\n$childOutput"
        }
    }
    foreach key {Total Passed Skipped Failed} value [list $total $passed $skipped $failed] {
        incr ::tcltest::numTests($key) $value
    }
    if {$failed != 0 || [string match {*Test files exiting with errors:*} $childOutput]} {
        puts [::tcltest::outputChannel] $childOutput
        if {$failed == 0} {error "logging regression child exited with test-file errors"}
    }
    return 1
}
