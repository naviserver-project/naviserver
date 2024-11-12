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

    proc http-0.9 {args} {
        ns_parseargs {
            {-encoding "utf-8"}
            {-http 1.0}
            -setheaders
            -getheaders
            -getmultiheaders
            {-getbody 0}
            {-getbinary 0}
            {-omitcontentlength 0}
            {-verbose 0}
            --
            method {url ""} {body ""}
        } $args

        set host [ns_config "test" loopback]
        set port [ns_config "ns/module/nssock" port]
        set timeout 3
        set state send
        set ::nstest::verbose $verbose

        #
        # Open a TCP connection to the host:port
        #

        lassign [ns_sockopen -nonblock $host $port] rfd wfd
        set sockerr [fconfigure $rfd -error]

        if {$sockerr ne {}} {
            return -code error $sockerr
        }

        #
        # Force network line ending semantics.
        #

        fconfigure $rfd -translation crlf -blocking 0
        fconfigure $wfd -translation crlf -blocking 1

        #
        # Force a specific encoding (utf-8 default).
        #
        if {$getbinary} {
            set encoding iso8859-1
        }

        fconfigure $rfd -encoding $encoding
        fconfigure $wfd -encoding $encoding

        if {[catch {

            #
            # User supplied headers.
            #

            set hdrs [ns_set create]
            if {[info exists setheaders]} {
                foreach {k v} $setheaders {
                    ns_set put $hdrs $k $v
                }
            }

            #
            # Default Headers (unless pre HTTP/1.0)
            #
            if {$http ne "" && $http >= "1.0"} {
                ns_set icput $hdrs accept */*
                ns_set icput $hdrs user-agent "[ns_info name]-Tcl/[ns_info version]"

                if {$http eq "1.0"} {
                    ns_set icput $hdrs connection close
                }

                if {[string match "*:*" $host]} {
                    set host "\[$host\]"
                }
                if {$port eq "80"} {
                    ns_set icput $hdrs host $host
                } else {
                    ns_set icput $hdrs host $host:$port
                }
            }

            if {$body ne {}} {
                set blen [string length $body]
                if {$omitcontentlength == 0} {
                    ns_set icput $hdrs content-length $blen
                }
            }

            #
            # Send the request.
            #

            if {$http eq ""} {
                set request "$method $url"
            } else {
                set request "$method $url HTTP/$http"
            }

            http_puts $timeout $wfd $request

            for {set i 0} {$i < [ns_set size $hdrs]} {incr i} {
                set key [ns_set key $hdrs $i]
                set val [ns_set value $hdrs $i]
                http_puts $timeout $wfd "$key: $val"
            }

            if {$http ne ""} {
                http_puts $timeout $wfd ""
            }
            flush $wfd
            log "flush header"

            if {$body ne {}} {
                fconfigure $wfd -translation binary -blocking 1
                http_write $timeout $wfd $body $blen
            }

            ns_set free $hdrs

            #
            # Read the response.
            #

            set state read
            set hdrs [ns_set create]
            set line [http_gets $timeout $rfd]

            if {[regexp {^HTTP.*([0-9][0-9][0-9]) .*$} $line -> response]} {

                #
                # A fully formed HTTP response.
                #

                while {1} {
                    set line [http_gets $timeout $rfd]
                    if {$line eq ""} {
                        break
                    }
                    ns_parseheader $hdrs $line
                }

                #
                # Read any body content.
                #

                set body ""
                set length [ns_set iget $hdrs content-length]
                if {$length eq {}} {
                    set length -1
                }
                set tencoding [ns_set iget $hdrs transfer-encoding]

                while {1} {
                    set buf [http_read $timeout $rfd $length]
                    set len [string length $buf]

                    if {$len == 0} {
                        break
                    }

                    append body $buf

                    if {$buf eq "0\n\n" && $tencoding eq "chunked"} {
                        break
                    }

                    if {$length > 0} {
                        set length [expr {$length - $len}]
                        if {$length <= 0} {
                            break
                        }
                    }
                }

            } else {

                #
                # Raw data.
                #

                set response ""
                set body $line
                append body [http_read $timeout $rfd -1]
            }

        } errMsg]} {

            #
            # For Bad requests we can still read the response
            #

            if {$state eq {read} && [info exists response]
                || ($state eq {send}
                    && [catch {set line [http_gets $timeout $rfd]}] == 0
                    && [regexp {^HTTP.*([0-9][0-9][0-9]) .*$} $line -> response])} {

                # OK

            } else {

                #
                # Something went wrong during the request, so return an error.
                #

                catch {close $rfd}
                catch {close $wfd}
                catch {ns_set free $hdrs}

                return -code error -errorinfo $errMsg
            }

        }

        #
        # Return the requested parts of the response.
        #
ns_log notice "XXXXX http-09 GETHEADERS"
        if {[info exists getheaders]} {
            foreach h $getheaders {
                lappend response [ns_set iget $hdrs $h]
            }
        }
ns_log notice "XXXXX http-09 GETHEADERS DONE"
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

        catch {close $rfd}
        catch {close $wfd}
        catch {ns_set free $hdrs}

        if {[string is true $getbody] && $body ne {}} {
            lappend response $body
        }

        if {[string is true $getbinary] && $body ne {}} {
            binary scan $body "H*" binary
            lappend response [regexp -all -inline {..} $binary]
        }

        return $response
    }

    proc http_gets {timeout sock} {
        http_readable $timeout $sock
        while {[gets $sock line] == -1} {
            if {[eof $sock]} {
                return -code error "http_gets: premature end of data"
            }
            http_readable $timeout $sock
        }
        log http_gets $line
        return $line
    }

    proc http_puts {timeout sock string} {
        log "http_puts" $string
        set ready [ns_sockselect -timeout $timeout {} $sock {}]
        if {[lindex $ready 1] eq {}} {
            return -code error "http_puts: write timed out"
        }

        puts $sock $string
    }

    proc http_readable {timeout sock} {
        set nread [ns_socknread $sock]
        if {$nread == 0} {
            set ready [ns_sockselect -timeout $timeout $sock {} {}]
            if {[lindex $ready 0] eq {}} {
                return -code error "http_readable: read timed out"
            }
            set nread [ns_socknread $sock]
        }
        log http_readable $nread
        return $nread
    }

    proc http_read {timeout sock length} {

        set nread [http_readable $timeout $sock]
        if {$nread == 0} {
            return ""
        }
        log "http_read <$nread> $length"

        if {$length > 0 && $length < $nread} {
            set nread $length
        }

        if {$length > -1} {
            log "http_read start-read $nread bytes"
            set result [read $sock $nread]
        } else {
            log "http_read start-read without length"
            set result [read $sock]
        }
        log "http_read returns" $result
        return $result
    }

    proc http_write {timeout sock string {length -1}} {

        set ready [ns_sockselect -timeout $timeout {} $sock {}]
        if {[lindex $ready 1] eq {}} {
            return -code error "http_puts: write timed out"
        }

        puts -nonewline $sock $string; flush $sock
        log "http_write" $string

        return

        #
        # Experimental/debugging block-wise write
        #

        if {$length == -1} {
            set length [string length $string]
        }

        set len [fconfigure $sock -buffersize]
        set beg 0
        set end [expr {$len - 1}]

        while {$beg < $length} {
            if {$end >= $length} {
                set end [expr {$length - 1}]
            }
            set ready [ns_sockselect -timeout $timeout {} $sock {}]
            if {[lindex $ready 1] eq {}} {
                return -code error "http_puts: write timed out"
            }
            puts -nonewline $sock [string range $string $beg $end]
            incr beg $len
            incr end $len
        }

        flush $sock
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
