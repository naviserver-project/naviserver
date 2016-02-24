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

    proc http {args} {
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

	    if {$http eq "1.0"} {
		ns_set icput $hdrs Connection close
	    }

	    if {$port eq "80"} {
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

	    http_puts $timeout $wfd ""
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


return
# 
# Below is an implementation of nstest::http based on "ns_http"
# instead of the low level socket commands above. The only difference
# is that the version below does not support modified encodings for
# sending an http requests (the importance is questionable).
#

# ::nstest::http -
#     Routines for opening HTTP connections through
#     the Tcl socket interface.
#

namespace eval ::nstest {

    proc http {args} {
	ns_parseargs {
	    {-http 1.0} 
	    -setheaders 
	    -getheaders
	    -encoding
	    -getmultiheaders 
	    {-getbody 0} 
	    {-getbinary 0}
	    {-verbose 0}
	    --
	    method {url ""} {body ""}
	} $args

	set host [ns_config "test" loopback]
	set port [ns_config "ns/module/nssock" port]
	set timeout 3
	set ::nstest::verbose $verbose

	#
	# We can't control currently the encoding of the request. Not
	# sure, of this is really needed.
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

	if {$http eq "1.0"} {
	    ns_set icput $hdrs Connection close
	}

	if {$port eq "80"} {
	    ns_set icput $hdrs Host $host
	} else {
	    ns_set icput $hdrs Host $host:$port
	}

	log url http://$host:$port/$url
	set r [ns_http queue -timeout $timeout -method $method -headers $hdrs http://$host:$port/$url]
	
	ns_set cleanup $hdrs 
	set hdrs [ns_set create]

	if {[string is true $getbinary]} {
	    set binaryFlag "-binary"
	} else {
	    set binaryFlag ""
	}
	
	ns_http wait {*}$binaryFlag -result body -status status -headers $hdrs $r
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
