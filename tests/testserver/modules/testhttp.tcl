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
#
# $Header$
#

# http.tcl -
#     Routines for opening HTTP connections through
#     the Tcl socket interface.
#


proc nstest_http {args} {
    ns_parseargs {
        -setheaders -getheaders {-getbody 0} {-http 1.0} --
        method {url ""} {body ""}
    } $args

    set host localhost
    set port [ns_config "ns/module/nssock" port]
    set timeout 10

    #
    # Open a TCP connection to the host:port
    #
    
    set fds [ns_sockopen -nonblock $host $port]
    set rfd [lindex $fds 0]
    set wfd [lindex $fds 1]

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
        ns_set icput $hdrs Connection close
        ns_set icput $hdrs User-Agent "[ns_info name]-Tcl/[ns_info version]"
        if {[string equal $port 80]} {
            ns_set icput $hdrs Host $host
        } else {
            ns_set icput $hdrs Host $host:$port
        }
        if {[string length $body] > 0} {
            ns_set icput $hdrs Content-Length [string length $body]
        }

        #
        # Send the request.
        #

        if {[string equal $http ""]} {
            set request "$method $url\r"
        } else {
            set request "$method $url HTTP/$http\r"
        }
        _ns_http_puts $timeout $wfd $request
        for {set i 0} {$i < [ns_set size $hdrs]} {incr i} {
            _ns_http_puts $timeout $wfd "[ns_set key $hdrs $i]: [ns_set value $hdrs $i]\r"
        }
        if {[string length $body] > 0} {
            _ns_http_puts $timeout $wfd "\r\n$body\r"
        } else {
            _ns_http_puts $timeout $wfd "\r"
        }
        catch {ns_set free $hdrs}
        catch {close $wfd}

        #
        # Read the response.
        #

        set hdrs [ns_set create]
        set line [_ns_http_gets $timeout $rfd]

        if {[regexp {^HTTP.*([0-9][0-9][0-9]) .*$} $line -> response]} {

            #
            # A fully formed HTTP response.
            #

            while {1} {
                set line [_ns_http_gets $timeout $rfd]
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
            if {[string equal $length ""]} {
                set length -1
            }
            while {1} {
                set buf [_ns_http_read $timeout $rfd $length]
                append body $buf
                if {[string equal $buf ""]} {
                    break
                }
                if {$length > 0} {
                    incr length -[string length $buf]
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
            append body [_ns_http_read $timeout $rfd -1]
        }


        catch {close $rfd}

    } errMsg]} {

        #
        # For Bad requests we can still read the response
        #
        if {![catch { set line [_ns_http_gets $timeout $rfd] }]} {
            if {[regexp {^HTTP.*([0-9][0-9][0-9]) .*$} $line -> response]} {
                return $response
            }
        }

        #
        # Something went wrong during the request, so return an error.
        #

        global errorInfo
        catch {close $wfd}
        catch {close $rfd}
        catch {ns_set free $hdrs}
        return -code error -errorinfo $errorInfo $errMsg

    }

    #
    # Return the requested parts of the response.
    #

    if {[info exists getheaders]} {
        foreach h $getheaders {
            lappend response [ns_set iget $hdrs $h]
        }
    }
    catch {ns_set free $hdrs}
    if {[string is true $getbody] && $body ne ""} {
        lappend response $body
    }

    return $response
}
