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

# $Header$

#
# sendmail.tcl - Define the ns_sendmail procedure for sending
# email from a Tcl script through a remote SMTP server.
#

proc _ns_smtp_send { mode wfp string timeout } {

    if {[ns_config -bool ns/parameters "smtplogmode" false]} {
        ns_log notice "S: $mode $wfp $string $timeout"
        return ""
    }

    if {[lindex [ns_sockselect -timeout $timeout {} $wfp {}] 1] == ""} {
        error "$mode: Timeout writing to SMTP host"
    }
    puts $wfp $string\r
    flush $wfp
}

proc _ns_smtp_recv { mode rfp check timeout { error 1 } } {

    if {[ns_config -bool ns/parameters "smtplogmode" false]} {
        return ""
    }

    while (1) {
      if {[lindex [ns_sockselect -timeout $timeout $rfp {} {}] 0] == ""} {
        error "$mode: Timeout reading from SMTP host"
      }
      set line [gets $rfp]
      set code [string range $line 0 2]
      if { ![string match $check $code] } {
        set errmsg "$mode: Expected a $check status line; got:\n$line"
        if { $error } { error $errmsg }
        ns_log Error ns_sendmail: $errmsg
        break
      }
      if ![string match "-" [string range $line 3 3]] { break }
    }
}

proc ns_sendmail_config { { mode "" } } {
    set myset [ns_set create smtpconfiguration \
    smtphost          [ns_config ns/parameters smtphost] \
    smtpport          [ns_config ns/parameters smtpport] \
    smtptimeout       [ns_config ns/parameters smtptimeout] \
    smtplogmode       [ns_config ns/parameters smtplogmode] \
    smtpmsgid         [ns_config ns/parameters smtpmsgid] \
    smtpmsgidhostname [ns_config ns/parameters smtpmsgidhostname] \
    smtpencodingmode  [ns_config ns/parameters smtpencodingmode] \
    smtpencoding      [ns_config ns/parameters smtpencoding] \
    smtpauthuser      [ns_config ns/parameters smtpauthuser] \
    smtpauthpassword  [ns_config ns/parameters smtpauthpassword]]
    if { $mode eq "log" } {
        ns_log notice [ns_set print $myset]
        return
    }
    return [ns_set array $myset]
}

proc _ns_sendmail_qp { str } {

   # first step: quote characters where necessary 
   regsub -all -- \
       {[\x00-\x08\x0B-\x1E\x21-\x24\x3D\x40\x5B-\x5E\x60\x7B-\xFF]} \
       $str {[format =%02X [scan "\\&" %c]]} str
   set str [subst -novariable $str]

   # second step: handle some special cases
   set _map [list "\t\n" "=09\n" " \n" "=20\n" "\n\.\n" "\n=2E\n"]

   return [string map $_map $str]

}

proc _ns_sendmail_breaklines { str } {

    set brokenlines ""
    foreach line [split $str "\n"] {
        while {[string length $line] > 72} {
            if {[regexp -indices -- {(=|=.)$} [string range $line 0 71] _findex]} {
                set _index_fragment [expr {[lindex $_findex 0] -1}]
                append brokenlines "[string range $line 0 $_index_fragment]=\n"
                incr _index_fragment +1
                set line [string range $line $_index_fragment end]
            } else {
                regexp -- {(.{72})(.*)} $line all first_72 line
                append brokenlines "${first_72}=\n"
            }
        }
        append brokenlines "${line}\n"
    }
    return [regsub -- {\n$} $brokenlines ""]

}

proc ns_sendmail { to from subject body {headers {}} {bcc {}} {cc {}} } {

    ## Takes comma-separated values in the "to,cc,bcc" parms
    ## Multiple To,CC and BCC addresses are handled appropriately.
    ## Original ns_sendmail functionality is preserved.

    ## Read CC/BCC addresses from extra headers if any
    if { $headers != "" } {
      if { [set addr [ns_set iget $headers cc]] != "" } {
        ns_set idelkey $headers cc
        append cc , $addr
      }
      if { [set addr [ns_set iget $headers bcc]] != "" } {
        ns_set idelkey $headers bcc
        append bcc , $addr
      }
    }

    ## Get smtp server into, if none then use localhost
    set smtp [ns_config ns/parameters smtphost]
    if [string match "" $smtp] { 
        set smtp [ns_config ns/parameters mailhost "localhost"] 
    }
    set timeout [ns_config ns/parameters smtptimeout "60"]
    set smtpport [ns_config ns/parameters smtpport "25"]
    ## Apply an encoding if configured; by default off
    if {[ns_config ns/parameters smtpencodingmode false]} {
        set target_encoding [ns_config ns/parameters smtpencoding "utf-8"]
        if {[string length $subject] != [string bytelength [encoding convertto $target_encoding $subject]]} {
            # just to satisfy pedantic spam rules (0.0 points anyway):
            # 0.0 SUBJECT_EXCESS_QP Subject: quoted-printable encoded unnecessarily
            set subject "=?$target_encoding?Q?[_ns_sendmail_qp [encoding convertto $target_encoding $subject]]?="
        }
        set body [_ns_sendmail_breaklines [_ns_sendmail_qp [encoding convertto $target_encoding $body]]]
        if { $headers == "" } {
            set headers [ns_set create headers]
            ns_set put $headers "MIME-Version" "1.0"
            ns_set put $headers "Content-Type" "text/plain; charset=\"${target_encoding}\"" 
            ns_set put $headers "Content-Transfer-Encoding" "quoted-printable" 
        } else {
            if {[ns_set iget $headers "content-type"] eq "" } {
                ns_set put $headers "Content-Type" "text/plain; charset=\"${target_encoding}\"" 
            }
            if {[ns_set iget $headers "content-transfer-encoding"] eq "" } {
                ns_set put $headers "Content-Transfer-Encoding" "quoted-printable" 
            }
            if {[ns_set iget $headers "mime-version"] eq ""} {
                ns_set put $headers "MIME-Version" "1.0"
            }
        }
    }

    ## Prepare to,cc,bcc address lists
    set tolist [list]
    regsub -all {[\n\r]} $to {} to
    foreach addr [split $to ,] {
      if { [set addr [string trim $addr]] != "" } { lappend tolist $addr }
    }

    set cclist [list]
    regsub -all {[\n\r]} $cc {} cc
    foreach addr [split $cc ,] {
      if { [set addr [string trim $addr]] != "" } { lappend cclist $addr }
    }
    
    set bcclist [list]
    regsub -all {[\n\r]} $bcc {} bcc
    foreach addr [split $bcc ,] {
      if { [set addr [string trim $addr]] != "" } { lappend bcclist $addr }
    }

    ## Send it along to _ns_sendmail
    _ns_sendmail $smtp $smtpport $timeout $tolist $cclist $bcclist \
	    $from $subject $body $headers
}

proc _ns_sendmail {smtp smtpport timeout tolist cclist bcclist from subject body headers} {

    ## Put the tolist in the headers
    set rfcto [join $tolist ", "]

    ## Build headers
    set _date [ns_httptime [ns_time]]

    set msg "To: $rfcto\nFrom: $from\nSubject: $subject\nDate: $_date\n"

    ## make sure we only work with the address itself from here on
    regexp {.*<(.*)>} $from null from

    ## CC recipients in separate header
    if { $cclist != "" } { append msg "Cc: [join $cclist ","]\n" }

    ## Insert extra headers, if any (not for BCC)
    if { $headers != "" } {
      set size [ns_set size $headers]
      for {set i 0} {$i < $size} {incr i} {
	append msg "[ns_set key $headers $i]: [ns_set value $headers $i]\n"
      }
    }
    ## If no Message-ID is specified, produce one if smtpmsgid is true
    ## See RFC2822(message identifier)
    if { [ns_config ns/parameters smtpmsgid false] && ($headers eq "" || \
             ($headers ne "" && [ns_set iget $headers "message-id"] eq ""))} {
        if {[set _msg_0 [ns_config ns/parameters smtpmsgidhostname]] eq ""} {
            set _msg_0 [ns_info hostname]
        }
	set _msg_1 [string range [ns_sha1 "$from$tolist$subject[clock seconds][ns_info threads]"] 0 14]
        set _msg_2 [clock format [clock seconds] -format "%Y%m%d"]
	set _msg_id "$_msg_1.$_msg_2@$_msg_0"
	append msg "Message-ID: <$_msg_id>\n"
    }

    ## Blank line between headers and body
    append msg "\n$body\n"
    
    ## Terminate body with a solitary period
    foreach line [split $msg "\n"] { 
      if { [string match . $line] } { append data . }
      append data $line
      append data "\r\n"
    }
    append data .

    ## Open the connection
    set sock [ns_sockopen $smtp $smtpport]
    set rfp [lindex $sock 0]
    set wfp [lindex $sock 1]

    ## Perform the (E)SMTP conversation
    if { [catch {
      _ns_smtp_recv "Start" $rfp 220 $timeout

      set _authuser [ns_config ns/parameters smtpauthuser]
      set _authpass [ns_config ns/parameters smtpauthpassword]
      set _hostname [ns_info hostname]
      if {[ns_config ns/parameters smtpmsgidhostname] ne ""} {
          set _hostname [ns_config ns/parameters smtpmsgidhostname]
      }
      if {$_authuser ne "" && $_authpass ne ""} {
          _ns_smtp_send "EHLO" $wfp "EHLO $_hostname" $timeout
          _ns_smtp_recv "EHLO" $rfp 250 $timeout

          if {[llength [split $_authuser "\0"]]==1} {
              # default case: equal user and realm
              set _authtoken "${_authuser}\0${_authuser}\0${_authpass}"
          } else {
              # self constructed user and realm
              set _authtoken "${_authuser}\0${_authpass}"
          }
          _ns_smtp_send "AUTH PLAIN" $wfp \
              "AUTH PLAIN [ns_base64encode $_authtoken]" $timeout
          _ns_smtp_recv "AUTH PLAIN" $rfp 235 $timeout
      } else {
          _ns_smtp_send "Helo" $wfp "HELO $_hostname" $timeout
          _ns_smtp_recv "Helo" $rfp 250 $timeout
      }

      _ns_smtp_send "Mail $from" $wfp "MAIL FROM:<$from>" $timeout
      _ns_smtp_recv "Mail $from" $rfp 250 $timeout
      ## Loop through To list via multiple RCPT TO lines
      foreach to $tolist {
        if { $to == "" } { continue }
        regexp {.*<(.*)>} $to null to
        _ns_smtp_send "Rcpt $to" $wfp "RCPT TO:<$to>" $timeout
        _ns_smtp_recv "Rcpt $to" $rfp 250 $timeout 0
      }

      ## Loop through CC list via multiple RCPT TO lines
      foreach to $cclist {
        if { $to == "" } { continue }
        regexp {.*<(.*)>} $to null to
        _ns_smtp_send "Rcpt $to" $wfp "RCPT TO:<$to>" $timeout
	_ns_smtp_recv "Rcpt $to" $rfp 250 $timeout 0
      }

      ## Loop through BCC list via multiple RCPT TO lines
      ## A BCC should never, ever appear in the header.  Ever.  Not even.
      foreach to $bcclist {
        if { $to == "" } { continue }
        regexp {.*<(.*)>} $to null to
        _ns_smtp_send "Rcpt $to" $wfp "RCPT TO:<$to>" $timeout
        _ns_smtp_recv "Rcpt $to" $rfp 250 $timeout 0
      }

      _ns_smtp_send Data $wfp DATA $timeout
      _ns_smtp_recv Data $rfp 354 $timeout
      _ns_smtp_send Data $wfp $data $timeout
      _ns_smtp_recv Data $rfp 250 $timeout
      _ns_smtp_send Quit $wfp QUIT $timeout
      _ns_smtp_recv Quit $rfp 221 $timeout 0
    } errMsg ] } {
      ## Error, close and report
      close $rfp
      close $wfp
      return -code error $errMsg
    }

    ## Close the connection
    close $rfp
    close $wfp
}

