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
# sendmail.tcl - Define the ns_sendmail procedure for sending
# email from a Tcl script through a remote SMTP server.
#

proc _ns_smtp_send { mode wfp string timeout} {
 
   if {[lindex [ns_sockselect -timeout $timeout {} $wfp {}] 1] == ""} {
     error "$mode: Timeout writing to SMTP host"
    }
    puts $wfp $string\r
    flush $wfp
}

proc _ns_smtp_recv { mode rfp check timeout { error 1 } } {
    
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
    if [string match "" $smtp] { set smtp [ns_config ns/parameters mailhost] }
    if [string match "" $smtp] { set smtp localhost }
    set timeout [ns_config ns/parameters smtptimeout]
    if [string match "" $timeout] { set timeout 60 }
    set smtpport [ns_config ns/parameters smtpport]
    if [string match "" $smtpport] { set smtpport 25 }

    ## Extract "from" email address
    if [regexp {.*<(.*)>} $from d address] { set from $address }
    
    ## Prepare to,cc,bcc address lists
    set tolist [list]
    regsub -all {[\n\r ]} $to {} to
    foreach addr [split $to ,] {
      if { [regexp {.*<(.*)>} $addr d address] } { set addr $address }
      if { [set addr [string trim $addr]] != "" } { lappend tolist $addr }
    }

    set cclist [list]
    regsub -all {[\n\r ]} $cc {} cc
    foreach addr [split $cc ,] {
      if { [regexp {.*<(.*)>} $addr d address] } { set addr $address }
      if { [set addr [string trim $addr]] != "" } { lappend cclist $addr }
    }
    
    set bcclist [list]
    regsub -all {[\n\r ]} $bcc {} bcc
    foreach addr [split $bcc ,] {
      if { [regexp {.*<(.*)>} $addr d address] } { set addr $address }
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
    set msg "To: $rfcto\nFrom: $from\nSubject: $subject\nDate: [ns_httptime [ns_time]]\n"

    ## CC recipients in separate header
    if { $cclist != "" } { append msg "Cc: [join $cclist ","]\n" }

    ## Insert extra headers, if any (not for BCC)
    if { $headers != "" } {
      set size [ns_set size $headers]
      for {set i 0} {$i < $size} {incr i} {
	append msg "[ns_set key $headers $i]: [ns_set value $headers $i]\n"
      }
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

    ## Perform the SMTP conversation
    if { [catch {
      _ns_smtp_recv "Start" $rfp 220 $timeout
      _ns_smtp_send "Helo" $wfp "HELO [ns_info hostname]" $timeout
      _ns_smtp_recv "Helo" $rfp 250 $timeout
      _ns_smtp_send "Mail $from" $wfp "MAIL FROM:<$from>" $timeout
      _ns_smtp_recv "Mail $from" $rfp 250 $timeout
	
      ## Loop through To list via multiple RCPT TO lines
      foreach to $tolist {
        if { $to == "" } { continue }
        _ns_smtp_send "Rcpt $to" $wfp "RCPT TO:<$to>" $timeout
        _ns_smtp_recv "Rcpt $to" $rfp 250 $timeout 0
      }

      ## Loop through CC list via multiple RCPT TO lines
      foreach to $cclist {
        if { $to == "" } { continue }
        _ns_smtp_send "Rcpt $to" $wfp "RCPT TO:<$to>" $timeout
	_ns_smtp_recv "Rcpt $to" $rfp 250 $timeout 0
      }
	
      ## Loop through BCC list via multiple RCPT TO lines
      ## A BCC should never, ever appear in the header.  Ever.  Not even.
      foreach to $bcclist {
        if { $to == "" } { continue }
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

