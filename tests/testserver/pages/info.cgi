# -*- Tcl -*-
# Check all the Meta-Variables as specified in
# https://datatracker.ietf.org/doc/html/rfc3875#section-4.1
#
# Note, this is a plain Tcl scripts, no NaviServer commands are
# allowed.
#
set done 0
if {[info exists ::env(QUERY_STRING)]} {
    set query ""
    set content ""
    set header ""
    foreach spec [split $::env(QUERY_STRING) &] {
        lassign [split $spec =] var value
        dict set query $var $value
    }
    if {[dict exists $query var]} {
        set var [dict get $query var]
        puts "Content-type: text/plain"
        puts ""
        if {[info exists ::env($var)]} {
            puts "$var: <$::env($var)>"
        } else {
            puts "$var does not exist"
        }
        set done 1
    }

    if {[dict exists $query status]} {
        lappend header "Status: [dict get $query status]"
    }
    if {[dict exists $query location]} {
        lappend header "Location: [dict get $query location]"
    }
    if {[dict exists $query content]} {
        set content [dict get $query content]
    }

    if {$header ne ""} {
        set reply [join $header \n]\n\n$content
        puts $reply
        set done 1
    }
    if {[dict exists $query rc]} {
        exit [dict get $query rc]
    }
}

if {!$done} {
    puts "Content-type: text/plain"
    puts ""

    set providedVarCount 0
    set varCount 0
    set missing 0
    set rfcVars {
        AUTH_TYPE 1
        CONTENT_LENGTH 1
        CONTENT_TYPE 1
        GATEWAY_INTERFACE 1
        PATH_INFO 1
        PATH_TRANSLATED 1
        QUERY_STRING 1
        REMOTE_ADDR 1
        REMOTE_HOST 1
        REMOTE_IDENT 0
        REMOTE_USER 1
        REQUEST_METHOD 1
        SCRIPT_NAME 1
        SERVER_NAME 1
        SERVER_PORT 1
        SERVER_PROTOCOL 1
        SERVER_SOFTWARE 1
    }
    foreach {v required} $rfcVars {
        incr varCount
        set varProvided [info exists ::env($v)]
        set line "[format %25s $v]: exists/required $varProvided/$required"
        if {$varProvided} {
            incr providedVarCount
            set provided($v) 1
            append line " <$::env($v)>"
        } elseif {$required} {
            incr missing
        }
        puts $line
    }
    puts "\n---------- RFC listed $varCount provided $providedVarCount missing $missing ---------\n"

    foreach v [lsort [array names ::env]] {
        if {$v ni $rfcVars} {
            puts "[format %25s $v]: <$::env($v)>"
        }
    }
}
