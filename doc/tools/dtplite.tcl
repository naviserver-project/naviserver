# Run dtplite with the Tcl interpreter selected by the build configuration.
if {[catch {package require dtplite 1.0.5} message]} {
    puts stderr "Cannot load Tcllib dtplite with [info nameofexecutable]: $message"
    puts stderr "Install Tcllib for this interpreter or set DTPLITE explicitly."
    exit 1
}
exit [dtplite::do $argv]
