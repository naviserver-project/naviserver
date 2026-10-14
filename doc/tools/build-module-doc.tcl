#!/usr/bin/env tclsh
# Standalone module documentation and optional combined-documentation staging.
# Uses only local sources; never fetches or checks out module repositories.
namespace eval ::ns_doc {
    variable tools [file dirname [file normalize [info script]]]
}

proc ::ns_doc::stage {source module destination} {
    if {![regexp {^[[:alnum:]][[:alnum:]_-]*$} $module]} {
        error "invalid module documentation name: $module"
    }
    set source [file normalize $source]
    set destination [file normalize $destination]
    set input [file join $source doc src]
    set fragment [file join $input include parameters.man]
    if {![file isdirectory $input] || ![file readable $fragment]} {
        error "missing module documentation or generated fragment: $fragment; run make regen-config-parameters-includes in $source"
    }
    # Selected repositories must supply a current committed fragment. The
    # check uses the same installed/shared generator and never rewrites it.
    variable tools
    set generator [file join $tools gen-config-parameters.tcl]
    exec [info nameofexecutable] $generator --config [file join $source doc tcl config-parameters.tcl] \
        --module $module --output $fragment --check 2>@1
    set target [file join $destination $module]
    if {[file exists $target]} {
        error "duplicate documentation module or staging collision: $target"
    }
    file mkdir $destination
    file copy $input $target
    # Preserve relative includes in the module manual. Also provide a separate
    # namespaced copy for the core parameter reference, with no source-tree copy.
    set includeDir [file join $destination include $module]
    file mkdir $includeDir
    file copy -force $fragment [file join $includeDir parameters.man]
    set manifest [open [file join $destination include config-parameters-external.man] a]
    try {
        puts $manifest "\[include include/$module/parameters.man\]"
    } finally {
        close $manifest
    }
    return $target
}

proc ::ns_doc::main {arguments} {
    set options [dict create --format html --module {} --source {} --output {} --stage {}]
    if {[llength $arguments]%2} {
        error "expected option/value pairs: --source DIR --module NAME (--output DIR ?--format html|nroff|validate? | --stage DIR)"
    }
    foreach {key value} $arguments {
        if {![dict exists $options $key]} {error "unknown option: $key"}
        dict set options $key $value
    }
    set source [dict get $options --source]
    set module [dict get $options --module]
    set destination [dict get $options --stage]
    set output [dict get $options --output]
    set format [dict get $options --format]
    if {$source eq "" || $module eq "" || ($destination eq "" && $output eq "")
        || ($destination ne "" && $output ne "") || $format ni {html nroff validate}} {
        error "specify --source, --module and either --stage or --output; format is html, nroff or validate"
    }
    if {$destination ne ""} {
        puts "Staged [stage $source $module $destination]"
        return
    }
    # All scratch files are owned by this invocation, outside the repositories.
    set channel [file tempfile temporary]
    close $channel
    file delete $temporary
    file mkdir $temporary
    try {
        set input [stage $source $module $temporary]
        package require dtplite
        if {$format eq "validate"} {
            package require fileutil
            set count 0
            foreach path [fileutil::find $input] {
                if {![file isfile $path] || [file extension $path] ne ".man"} {continue}
                set channel [open $path]
                try {set text [read $channel]} finally {close $channel}
                if {[string first {[manpage_begin } $text]<0} {continue}
                if {[dtplite::do [list validate $path]] != 0} {error "invalid manual: $path"}
                incr count
            }
            if {$count==0} {error "no complete module manuals in $input"}
        } else {
            set output [file normalize $output]
            file mkdir $output
            if {[dtplite::do [list -o $output $format $input]] != 0} {
                error "module documentation build failed: $module"
            }
        }
    } finally {
        file delete -force $temporary
    }
}

if {[file normalize [info script]] eq [file normalize $::argv0]} {
    if {[catch {::ns_doc::main $::argv} message]} {
        puts stderr $message
        exit 1
    }
}
