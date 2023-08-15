# -*- Tcl -*-
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
#

# ::nstest::http -
#     Routines for opening HTTP connections through
#     the Tcl socket interface.
#

namespace eval ::nstest {

    proc https {args} {
        return [request -proto https {*}$args]
    }
    proc http {args} {
        return [request -proto http {*}$args]
    }

    proc request {args} {
        ns_parseargs {
            {-proto http}
            {-http 1.0}
            {-setheaders}
            {-getheaders}
            {-getmultiheaders}
            {-getbody 0}
            {-getbinary 0}
            {-timeout 3s}
            {-partialresults 0}
            {-verbose 0}
            {-hostname}
            --
            method
            {url ""}
            {body ""}
        } $args

        set addr [ns_config "test" loopback]
        set host test
        switch $proto {
            "https" {
                set port [ns_config "ns/module/nsssl" port]
                set defaultPort 443
            }
            "http" {
                set port [ns_config "ns/module/nssock" port]
                set defaultPort 80
            }
            default {error "protocol $proto not supported"}
        }

        set ::nstest::verbose $verbose
        set extraFlags {}

        if {[info exists hostname]} {
            lappend extraFlags -hostname $hostname
            set host $hostname
        }
        if {$partialresults} {
            lappend extraFlags -partialresults
        }

        set hdrs [ns_set create]
        if {[info exists setheaders]} {
            foreach {k v} $setheaders {
                ns_set put $hdrs $k $v
            }
        }

        #
        # Default Headers.
        #

        ns_set icput $hdrs Accept */*
        ns_set icput $hdrs User-Agent "[ns_info name]-Tcl/[ns_info version]"

        if {$http eq "1.0"} {
            ns_set icput $hdrs Connection close
        }

        #
        # Add "Host:" header filed only, when not provided
        #
        if {[ns_set iget $hdrs host ""] eq ""} {
            if {$port eq $defaultPort} {
                ns_set icput $hdrs Host $host
            } else {
                if {[string match *:* $host]} {
                    ns_set icput $hdrs Host \[$host\]:$port
                } else {
                    ns_set icput $hdrs Host $host:$port
                }
            }
        } else {
            lappend extraFlags "-keep_host_header"
        }
        #ns_log notice "HEADERS [ns_set array $hdrs]"

        #if {$getbinary} {
        #    lappend extraFlags "-binary"
        #}

        set fullUrl $proto://\[$addr\]:$port/[string trimleft $url /]
        log url $fullUrl
        try {
            ns_http run \
                {*}$extraFlags \
                -timeout $timeout \
                -method $method \
                -headers $hdrs \
                -body $body \
                $fullUrl
        } trap {NS_TIMEOUT} {errorMsg} {
            #ns_log notice "REQUEST timeout: $errorMsg errorCode $::errorCode"
            dict set result status 000
            dict set result body "testcase NS_TIMEOUT: $errorMsg"
            dict set result headers [ns_set create]

        } on error {errorMsg} {
            #ns_log notice "REQUEST error: $errorMsg errorCode $::errorCode"
            ::throw $::errorCode $errorMsg
        } on ok {result} {
            #ns_log notice "REQUEST returned $result"
        }

        set body [dict get $result body]
        set status [dict get $result status]
        set hdrs [dict get $result headers]
        log status $status

        set response [list $status]

        if {[info exists getheaders]} {
            foreach h $getheaders {
                lappend response [ns_set iget $hdrs $h]
            }
        }
        if {[info exists getmultiheaders]} {
            foreach h $getmultiheaders {
                for {set i 0} {$i < [ns_set size $hdrs]} {incr i} {
                    set key [ns_set key $hdrs $i]
                    if {[string tolower $h] eq [string tolower $key]} {
                        lappend response [ns_set value $hdrs $i]
                    }
                }
            }
        }

        if {[string is true $getbody] && $body ne {}} {
            lappend response $body
        }

        if {[string is true $getbinary] && $body ne {}} {
            binary scan $body "H*" binary
            lappend response [regexp -all -inline {..} $binary]
        }

        return $response
    }

    proc log {what {msg ""}} {
        if {!$::nstest::verbose} {return}

        set length [string length $msg]
        if {$length > 40} {
            puts stderr "... $what: <[string range $msg 0 40]...> ($length bytes)"
        } else {
            puts stderr "... $what: <$msg>"
        }
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
