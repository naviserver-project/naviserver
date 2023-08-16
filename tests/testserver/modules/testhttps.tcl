# -*- Tcl -*-
#
# The contents of this file are subject to the AOLserver Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://aolserver.com/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is AOLserver Code and related documentation
# distributed by AOL.
#
# The Initial Developer of the Original Code is America Online,
# Inc. Portions created by AOL are Copyright (C) 1999 America Online,
# Inc. All Rights Reserved.
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
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

        if {[string is true $getbinary]} {
            set binaryFlag "-binary"
        } else {
            set binaryFlag ""
        }

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

        #ns_set cleanup $hdrs
        #set hdrs [ns_set create]

        #ns_http wait {*}$binaryFlag -result body -status status  -headers $hdrs $r
        #ns_log notice result=$result
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
