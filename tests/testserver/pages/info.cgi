# -*- Tcl -*-
# Check all the Meta-Variables as specified in
# https://datatracker.ietf.org/doc/html/rfc3875#section-4.1
#
puts "Content-type: text/plain"
puts ""
if {[info exists ::env(QUERY_STRING)]
    && [regexp {^var=(.*)$} $::env(QUERY_STRING) . var]
} {
    if {[info exists ::env($var)]} {
        puts "$var: $::env($var)"
    } else {
        puts "$var does not exist"
    }
} else {
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
