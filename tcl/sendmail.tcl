#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://www.mozilla.org/.
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
# sendmail.tcl --
#
#   Support for sending email from a Tcl script
#   to a remote SMTP server.
#


#
# ns_sendmail --
#
#   Sends the email to remote SMTP server.
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_sendmail args {
    
    lassign $args to from subject body headers bcc cc
    if {![string match -* $to]} {
        ns_log warning "Deprecated syntax. Use: [list ns_sendmail -to $to -from $from -subject $subject -body $body -headers $headers -bcc $bcc -cc $cc]"
    } else {
        ns_parseargs {
            {-to ""}
            {-from ""}
            {-subject ""}
            {-body ""}
            {-headers ""}
            {-bcc ""}
            {-cc ""}
        } $args
    }

    #
    # Flag: need to cleanup after ourselves
    #

    set cleanup 0

    #
    # Assure subject is always a one-liner
    #

    set subject [string trim [string map {\n "" \r ""} $subject]]

    #
    # Read Cc/Bcc addresses from extra headers and remove
    # them from there, as we are handling them separately.
    #

    if {$headers ne ""} {
        foreach key [list cc bcc] {
            set addr [ns_set iget $headers $key]
            if {$addr ne ""} {
                ns_set idelkey $headers $key
                if {[set $key] ne ""} {
                    append $key ","
                }
                append $key $addr
            }
        }
    }

    #
    # Prepare To: address list
    #

    set tolist [list]
    foreach addr [string trim [split [string map {\n "" \r ""} $to] ","]] {
        if {$addr ne ""} {
            lappend tolist $addr
        }
    }

    #
    # Prepare Cc: address list
    #

    set cclist [list]
    foreach addr [string trim [split [string map {\n "" \r ""} $cc] ","]] {
        if {$addr ne ""} {
            lappend cclist $addr
        }
    }

    #
    # Prepare Bcc: address list
    #

    set bcclist [list]
    foreach addr [string trim [split [string map {\n "" \r ""} $bcc] ","]] {
        if {$addr ne ""} {
            lappend bcclist $addr
        }
    }

    #
    # Apply encoding on subject, body, if configured
    #

    if {[ns_config -set ns/parameters smtpencodingmode false]} {
        set encoding [ns_config -set ns/parameters smtpencoding "utf-8"]
        set quotemsg 0

        set cbody [encoding convertto $encoding $body]
        set csubj [encoding convertto $encoding $subject]

        if {[string length $subject] != [string bytelength $csubj]} {
            set quotemsg 1
            set subject "=?$encoding?Q?[_ns_sendmail_qp $csubj]?="
        }
        if {[string length $body] != [string bytelength $cbody]} {
            set quotemsg 1
            set body [_ns_sendmail_breaklines [_ns_sendmail_qp $cbody]]
        }
        if {$quotemsg} {
            if {$headers eq ""} {
                set cleanup 1
                set headers [ns_set create headers]
            }
            set key "MIME-version"
            if {[ns_set iget $headers $key] eq ""} {
                ns_set put $headers $key "1.0"
            }
            set key "Content-Type"
            if {[ns_set iget $headers $key] eq ""} {
                ns_set put $headers $key "text/plain; charset=\"${encoding}\""
            }
            set key "Content-Transfer-Encoding"
            if {[ns_set iget $headers $key] eq ""} {
                ns_set put $headers $key "quoted-printable"
            }
        }
    }

    #
    # Put custom headers
    #

    if {$headers ne ""} {
        for {set i 0} {$i < [ns_set size $headers]} {incr i} {
            set key [ns_set key   $headers $i]
            set val [ns_set value $headers $i]
            append msg $key ": " $val \n
        }
    }

    #
    # Put message essentials
    #

    set date  [ns_httptime [clock seconds]]
    set rfcto [join $tolist ", "]

    append msg "To: "      $rfcto   \n
    append msg "From: "    $from    \n
    append msg "Subject: " $subject \n
    append msg "Date: "    $date    \n

    #
    # Put Cc: recipients in separate header
    #

    if {$cclist ne ""} {
        append msg "Cc: " [join $cclist ","] \n
    }

    #
    # Make sure we only work with the address itself from here on
    #

    regexp {.*<(.*)>} $from null from

    #
    # If no Message-ID is specified, produce one optionally
    # See RFC2822 (message identifier)
    #

    set host [ns_config ns/parameters smtpmsgidhostname]
    if {$host eq ""} {
        set host [ns_info hostname]
    }

    if {[ns_config -set ns/parameters smtpmsgid false] &&
        ($headers eq "" || [ns_set iget $headers "Message-ID"] eq "")} {
        set threads [ns_info threads]
        set nowsecs [clock seconds]
        set shabang [ns_sha1 "$from$tolist$subject$nowsecs$threads"]

        set idpart1 [string range $shabang 0 14]
        set idpart2 [clock format $nowsecs -format "%Y%m%d"]

        append msg "Message-ID: <$idpart1.$idpart2@$host>" \n
    }

    #
    # Blank line between headers and body
    #

    append msg \n $body \n

    #
    # Terminate entire message with a solitary period
    # converting all "." on a single line to "..".
    #

    foreach line [split $msg "\n"] {
        if {$line eq {.}} {
            append data "."
        }
        append data $line \n
    }

    append data "."

    #
    # Get auth data for connection to SMTP. We just
    # blindly send this data to the remote server w/o
    # being asked for.
    # AUTH PLAIN and AUTH LOGIN are supported.
    #

    set authmode [string tolower [ns_config ns/parameters smtpauthmode]]
    set user [ns_config ns/parameters smtpauthuser]
    set pass [ns_config ns/parameters smtpauthpassword]

    set usestarttls [ns_config ns/parameters smtpusestarttls 0]
    set certfile [ns_config ns/parameters smtpcertfile]
    set cafile [ns_config ns/parameters smtpcafile]
    set cadir [ns_config ns/parameters smtpcadir]

    #
    # Open the connection to SMTP server
    #

    set smtphost [ns_config ns/parameters smtphost]
    set smtpport [ns_config -set ns/parameters smtpport 25]
    set timeout  [ns_config -set ns/parameters smtptimeout 60]

    if {$smtphost eq ""} {
        set smtphost [ns_config -set ns/parameters mailhost "localhost"]
    }

    lassign [ns_sockopen -timeout $timeout $smtphost $smtpport] rfd wfd
    fconfigure $wfd -translation crlf

    #
    # Perform the (E)SMTP conversation
    #

    set err [catch {

        _ns_smtp_recv "Start" $wfd 220
        _ns_smtp_send "EHLO" $wfd "EHLO $host"

        set lines [_ns_smtp_recv "EHLO" $wfd 250]

        if {$usestarttls} {

            #
            # STARTTLS as implemented by sendmail.tcl needs the Tcl
            # package tls, which has to be available on the load
            # path. Since the streams used here are Tcl streams these
            # can be upgraded to TLS using the stacked streams of the
            # tls module. This implementation uses select(). An
            # alternative implementation for STARTTLS exists in the
            # module nssmtpd, which provides a client and server side
            # implementation based on the NaviServer I/O
            # infrastructure.
            #
            package req tls

            if {$certfile eq ""} {
                ns_log error "ns_sendmail: param smtpcertfile must not be empty"
            } else {

                #
                # If STARTTLS is configured, first check if the server supports
                # it.
                #

                set hasStarttls 0
                foreach line $lines {
                    set command [string range $line 4 11]
                    if {$command eq "STARTTLS"} {
                        set hasStarttls 1
                    }
                }
                if {$hasStarttls == 0} {
                    ns_log warning "ns_sendmail: SMTP server does not support STARTTLS"
                } else {

                    #
                    # Request STARTTLS
                    #

                    _ns_smtp_send "STARTTLS" $wfd "STARTTLS"
                    _ns_smtp_recv "STARTTLS" $wfd 220

                    #
                    # Do the TLS handshake
                    #

                    tls::import $wfd -certfile $certfile -cadir $cadir      \
                        -cafile $cafile
                    tls::handshake $wfd

                    #
                    # Note: Translation MUST be reconfigured after the tls handshake
                    # because it is reset to some default!
                    #

                    fconfigure $wfd -translation crlf
                }
            }
        }

        #
        # Optionaly authorize (PLAIN or LOGIN)
        #

        if {$user ne "" && $pass ne ""} {
            if {[llength [split $user "\0"]] == 1} {
                # Default case: user and realm are same
                set token [ns_base64encode "${user}\0${user}\0${pass}"]
            } else {
                # Self constructed user and realm
                set token [ns_base64encode"${user}\0${pass}"]
            }

            #
            # Use AUTH PLAIN if no or no other mode is defined
            #
            if {$authmode in {"" PLAIN}} {

                _ns_smtp_send "AUTH PLAIN" $wfd "AUTH PLAIN $token"
                _ns_smtp_recv "AUTH PLAIN" $wfd 235

            } elseif {$authmode eq "LOGIN"} {

                _ns_smtp_send "AUTH LOGIN" $wfd "AUTH LOGIN"
                _ns_smtp_recv "AUTH LOGIN" $wfd 334
                _ns_smtp_send "AUTH LOGIN" $wfd [ns_base64encode $user]
                _ns_smtp_recv "AUTH LOGIN" $wfd 334
                # then send password
                _ns_smtp_send "AUTH LOGIN" $wfd [ns_base64encode $pass]
                _ns_smtp_recv "AUTH LOGIN" $wfd 235
            }
        }

        _ns_smtp_send "Mail $from" $wfd "MAIL FROM:<$from>"
        _ns_smtp_recv "Mail $from" $wfd 250

        #
        # Tell remote server about recipients. Count all
        # aknowledged ones
        #

        set countok 0

        foreach to [concat $tolist $cclist $bcclist] {
            regexp {<(.+)>} $to . to
            if {$to ne ""} {
                _ns_smtp_send "Rcpt $to" $wfd "RCPT TO:<$to>"
                if {![catch {_ns_smtp_recv "Rcpt $to" $wfd 250}]} {
                    incr countok
                }
            }
        }

        #
        # Send data only if got at least
        # one acknowledged recipient.
        #

        if {$countok > 0} {
            _ns_smtp_send Data $wfd DATA
            _ns_smtp_recv Data $wfd 354
            _ns_smtp_send Data $wfd $data
            _ns_smtp_recv Data $wfd 250
        }

        _ns_smtp_send Quit $wfd QUIT
        _ns_smtp_recv Quit $wfd 221 0

    } errmsg]

    close $rfd
    close $wfd

    if {$cleanup} {
        ns_set free $headers
    }

    if {$err} {
        return -code error $errmsg
    }
}


#
# ns_sendmail_config --
#
#   Returns current SMTP parameters as key/value list.
#
# Result:
#   Key/value list.
#
# Side effects:
#   None.
#

proc ns_sendmail_config {{mode ""}} {

    set myset                                                              \
        [ns_set create smtpconfiguration                                   \
             smtphost          [ns_config ns/parameters smtphost]          \
             smtpport          [ns_config ns/parameters smtpport]          \
             smtptimeout       [ns_config ns/parameters smtptimeout]       \
             smtplogmode       [ns_config ns/parameters smtplogmode]       \
             smtpmsgid         [ns_config ns/parameters smtpmsgid]         \
             smtpmsgidhostname [ns_config ns/parameters smtpmsgidhostname] \
             smtpencodingmode  [ns_config ns/parameters smtpencodingmode]  \
             smtpencoding      [ns_config ns/parameters smtpencoding]      \
             smtpauthmode      [ns_config ns/parameters smtpauthmode]      \
             smtpauthuser      [ns_config ns/parameters smtpauthuser]      \
             smtpauthpassword  [ns_config ns/parameters smtpauthpassword]  \
             smtpusestarttls   [ns_config ns/parameters smtpusestarttls]   \
             smtpcertfile      [ns_config ns/parameters smtpcertfile]      \
             smtpcafile        [ns_config ns/parameters smtpcafile]        \
             smtpcadir         [ns_config ns/parameters smtpcadir]]

    if {$mode eq {log}} {
        ns_log notice [ns_set print $myset]
        set keyval ""
    } else {
        set keyval [ns_set array $myset]
    }

    ns_set free $myset

    return $keyval
}

#
# _ns_sendmail_qp --
#
#   Encode the string using quoted-printable encoding.
#
# Result:
#   String in quoted-printable format
#
# Side effects:
#   None.
#

proc _ns_sendmail_qp {str} {

    #
    # Quote characters where necessary
    #

    set pat {[\x00-\x08\x0B-\x1E\x21-\x24\x3D\x40\x5B-\x5E\x60\x7B-\xFF]}
    set str [regsub -all -- $pat $str {[format =%02X [scan "\\&" %c]]}]
    set str [subst -novariable $str]

    #
    # Handle some special cases
    #

    set map [list "\t\n" "=09\n" " \n" "=20\n" "\n\.\n" "\n=2E\n"]

    return [string map $map $str]
}

#
# _ns_sendmail_breaklines --
#
#   Assures lines in the body of the message do not
#   exceed 72 characters. Lines longer than that are
#   wrapped onto next line with an "=" on line end.
#
# Result:
#   String with lines broken on 72 margin.
#
# Side effects:
#   None.
#

proc _ns_sendmail_breaklines {string} {

    set broken ""

    #
    # Break lines on 72 margin boundary.
    # Watch not to break in the middle
    # of quoted-printable-encoded chars.
    #

    foreach line [split $string "\n"] {
        while {[string length $line] > 72} {
            set chunk [string range $line 0 72]
            if {[regexp -- (=|=.)$ $chunk dummy end]} {
                # Don't break in the middle of a code
                set len [expr {72 - [string length $end]}]
                set chunk [string range $line 0 $len]
                incr len
                set line [string range $line $len end]
            } else {
                set line [string range $line 73 end]
            }
            append broken $chunk= \n
        }
        append broken $line \n
    }

    #
    # The side-effect of the above is a trailing
    # newline, so junk it here
    #

    set broken [string range $broken 0 end-1]
}


#
# _ns_smtp_send --
#
#   Send a string to SMTP server
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc _ns_smtp_send {mode sock string} {

    if {[ns_config -bool -set ns/parameters smtplogmode false]} {
        ns_log notice "S: $mode $sock $string"
        return ""
    }

    set tout [ns_config -set ns/parameters smtptimeout 60]

    foreach line [split $string "\n"] {
        set fds [ns_sockselect -timeout $tout {} $sock {}]
        if {[lindex $fds 1] eq ""} {
            return -code error "$mode: Timeout writing to SMTP host"
        }
        puts $sock $line
    }
    flush $sock
}


#
# _ns_smtp_recv --
#
#   Receive line(s) from SMTP server and check against the
#   constraints.
#
# Result:
#   The list of lines if any.
#
# Side effects:
#   Depeding on the "error" flag, may or may not throw
#   Tcl error on constraint test failure. Regardless
#   of that, it always logs the failure to the server log.
#

proc _ns_smtp_recv {mode sock check {error 1}} {

    if {[ns_config -bool -set ns/parameters smtplogmode false]} {
        ns_log notice "R: $mode $sock $check"
        return ""
    }

    set tout [ns_config -set ns/parameters smtptimeout 60]
    set lines [list]

    while (1) {
        set fds [ns_sockselect -timeout $tout $sock {} {}]
        if {[lindex $fds 0] eq ""} {
            return -code error "$mode: timeout reading from SMTP host"
        }
        if {[gets $sock line] == -1} {
            if {[eof $sock]} {
                return -code error "$mode: remote peer closed connection"
            }
        } else {
            #puts stderr "#### _ns_smtp_recv reveived <$line>"
            #
            # Examine line of code returned by the server.
            # Normally the line has this form:
            #
            #  250 Develop Hello [192.168.234.100], pleased to meet you
            #
            # We are checking the first 3 digits of the code.
            # If server needs to generate more lines, it will
            # look like this:
            #
            #  214-2.0.0 For more info use "HELP <topic>".
            #  214-2.0.0 To report bugs in the implementation
            #  214-2.0.0 contact Technical Support.
            #  214-2.0.0 For local information send email to Postmaster
            #  214 2.0.0 End of HELP info
            #
            # Here, every line except the last one has "-" after
            # the status code.
            #

            lappend lines $line

            set code [string range $line 0 2]
            if {![string match $check $code]} {
                set errmsg "$mode: expected $check status line; got: $line"
                if {$error} {
                    return -code error $errmsg
                }
                ns_log error "ns_sendmail: $errmsg"
                break
            }
            if {[string index $line 3] ne "-"} {
                break ; # Terminal line, stop.
            }
        }
    }

    return $lines
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
