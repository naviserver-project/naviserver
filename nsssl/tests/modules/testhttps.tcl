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
	ns_parseargs {
	    {-http 1.0} 
	    -setheaders 
	    -getheaders 
	    -getmultiheaders 
	    {-getbody 0} 
	    {-getbinary 0}
	    {-verbose 0}
	    --
	    method {url ""} {body ""}
	} $args

	set host localhost
	set port [ns_config "ns/server/test/module/nsssl" port]
	set timeout 3
	set ::nstest::verbose $verbose

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
	if {[string is true $getbinary]} {
	    set binaryFlag "-binary"
	} else {
	    set binaryFlag ""
	}
	
	log url https://$host:$port/$url
	set r [ns_http queue -timeout $timeout -method $method -headers $hdrs https://$host:$port/$url]
	
	ns_set cleanup $hdrs 
	set hdrs [ns_set create]
	
	ns_http wait {*}$binaryFlag -result body -status status  -headers $hdrs $r
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
