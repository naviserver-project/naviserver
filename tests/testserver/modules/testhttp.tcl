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

# http.tcl -
#     Routines for opening HTTP connections through
#     the Tcl socket interface.
#


proc nstest_http {args} {
    ns_parseargs {
        {-encoding "utf-8"} -setheaders -getheaders {-getbody 0} {-getbinary 0} {-omitcontentlength 0} {-http 1.0} --
        method {url ""} {body ""}
    } $args

    set host localhost
    set port [ns_config "ns/module/nssock" port]
    set timeout 3
    set state send

    #
    # Open a TCP connection to the host:port
    #
    
    set fds [ns_sockopen -nonblock $host $port]
    set rfd [lindex $fds 0]
    set wfd [lindex $fds 1]

    set sockerr [fconfigure $rfd -error]
    if {$sockerr ne {}} {
        return -code error $sockerr
    }

	#
	# Force network line ending symantics.
	#

    fconfigure $rfd -translation crlf -blocking 0
    fconfigure $wfd -translation crlf -blocking 1

	#
	# Force a specific encoding (utf-8 default).
	#

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
        # Default Headers.
        #

        ns_set icput $hdrs Accept */*
        ns_set icput $hdrs User-Agent "[ns_info name]-Tcl/[ns_info version]"

		if {[string equal $http 1.0]} {
			ns_set icput $hdrs Connection close
		}

        if {[string equal $port 80]} {
            ns_set icput $hdrs Host $host
        } else {
            ns_set icput $hdrs Host $host:$port
        }

        if {$body ne {}} {
            set blen [string length $body]
	    if {$omitcontentlength == 0} {
		ns_set icput $hdrs Content-Length $blen
	    }
        }

        #
        # Send the request.
        #

        if {[string equal $http ""]} {
            set request "$method $url"
        } else {
            set request "$method $url HTTP/$http"
        }

        nstest_http_puts $timeout $wfd $request

        for {set i 0} {$i < [ns_set size $hdrs]} {incr i} {
            set key [ns_set key $hdrs $i]
            set val [ns_set value $hdrs $i]
            nstest_http_puts $timeout $wfd "$key: $val"
        }

        nstest_http_puts $timeout $wfd ""
        flush $wfd

        if {$body ne {}} {
            fconfigure $wfd -translation binary -blocking 1
            nstest_http_write $timeout $wfd $body $blen
        }

        ns_set free $hdrs

        #
        # Read the response.
        #

        set state read
        set hdrs [ns_set create]
        set line [nstest_http_gets $timeout $rfd]

        if {[regexp {^HTTP.*([0-9][0-9][0-9]) .*$} $line -> response]} {

            #
            # A fully formed HTTP response.
            #

            while {1} {
                set line [nstest_http_gets $timeout $rfd]
                if {![string length $line]} {
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
                set buf [nstest_http_read $timeout $rfd $length]
                set len [string length $buf]

				if {$len == 0} {
					break
				}

                append body $buf

				if {[string equal $buf "0\n\n"] && [string equal $tencoding chunked]} {
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
            append body [nstest_http_read $timeout $rfd -1]
        }

    } errMsg]} {

        #
        # For Bad requests we can still read the response
        #

        if {$state eq {read} && [info exists response]
            || ($state eq {send}
                && [catch {set line [nstest_http_gets $timeout $rfd]}] == 0
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

    if {[info exists getheaders]} {
        foreach h $getheaders {
            lappend response [ns_set iget $hdrs $h]
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

proc nstest_http_gets {timeout sock} {

    while {[gets $sock line] == -1} {
        if {[eof $sock]} {
            return -code error "nstest_http_gets: premature end of data"
        }
        nstest_http_readable $timeout $sock
    }

    return $line
}

proc nstest_http_puts {timeout sock string} {

    set ready [ns_sockselect -timeout $timeout {} $sock {}]
    if {[lindex $ready 1] eq {}} {
        return -code error "nstest_http_puts: write timed out"
    }

    puts $sock $string
}

proc nstest_http_readable {timeout sock} {

    set nread [ns_socknread $sock]
    if {$nread == 0} {
        set ready [ns_sockselect -timeout $timeout $sock {} {}]
        if {[lindex $ready 0] eq {}} {
            return -code error "nstest_http_readable: read timed out"
        }
        set nread [ns_socknread $sock]
    }

    return $nread
}

proc nstest_http_read {timeout sock length} {
	
	set nread [nstest_http_readable $timeout $sock]
	if {$nread == 0} {
		return ""
	}

	if {$length > 0 && $length < $nread} {
		set nread $length
	}

	if {$length > -1} {
		return [read $sock $nread]
	} else {
		return [read $sock]
	}
}

proc nstest_http_write {timeout sock string {length -1}} {

     set ready [ns_sockselect -timeout $timeout {} $sock {}]
     if {[lindex $ready 1] eq {}} {
         return -code error "nstest_http_puts: write timed out"
     }
 
     puts -nonewline $sock $string; flush $sock

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
            return -code error "nstest_http_puts: write timed out"
        }
        puts -nonewline $sock [string range $string $beg $end]
        incr beg $len
        incr end $len
    }

    flush $sock
}
