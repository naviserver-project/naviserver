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
# nsconfig.tcl --
#
#   Set of procedures implementing simple nsd.tcl configurator
#   which takes runtime config and allows to modify and re-create new nsd.tcl file or
#   even replace existing one if priviliges allow.
#
#   To use it, just drop it somewehere under naviserver pageroot which is usually
#   /usr/local/ns/pages and point browser to it
#

# If this page needs to be restricted assign username and password here
set user ""
set password ""

# Current server
set server [ns_info server]

# Initialize help entries
if { ![nsv_exists _ns_config server.$server] } {

  nsv_set _ns_config server.$server 1
  nsv_set _ns_config help.ns/servers.* "Server name if config is shared between multiple servers"

  nsv_set _ns_config help.ns/mimetypes.* "Mime type to return for file extension"

  nsv_set _ns_config help.ns/threads.stacksize "Stack size for every new thread, should be big enough to avoid crashes"

  nsv_set _ns_config help.ns/server/$server/modules.* "Module to load on server startup"

  nsv_set _ns_config help.ns/server/$server/tcl.errorlogheaders "List of connection headers to log for Tcl errors"
  nsv_set _ns_config help.ns/server/$server/tcl.initfile "Tcl file to perform initialization, init.tcl by default"
  nsv_set _ns_config help.ns/server/$server/tcl.library "Where all private Tcl modules are located"
  nsv_set _ns_config help.ns/server/$server/tcl.lazyloader "Set to true to use Tcl-trace based interp initialization"
  nsv_set _ns_config help.ns/server/$server/tcl.nsvbuckets "Number of buckets in Tcl hash table for nsv vars"

  nsv_set _ns_config help.ns/server/$server/redirects.* "HTTP return code redirect url"

  nsv_set _ns_config help.ns/server/$server/module/nssock.port "TCP port server will listen on"
  nsv_set _ns_config help.ns/server/$server/module/nssock.address "IP address for listener to bind on"
  nsv_set _ns_config help.ns/server/$server/module/nssock.hostname "Hostname to use in redirects"
  nsv_set _ns_config help.ns/server/$server/module/nssock.maxinput "Max upload size"
  nsv_set _ns_config help.ns/server/$server/module/nssock.maxline "Max line size in request or HTTP header"
  nsv_set _ns_config help.ns/server/$server/module/nssock.bufsize "Read-ahead buffer size"
  nsv_set _ns_config help.ns/server/$server/module/nssock.readahead "Max upload size when to use spooler"
  nsv_set _ns_config help.ns/server/$server/module/nssock.uploadsize "Max upload size when to use statistics"
  nsv_set _ns_config help.ns/server/$server/module/nssock.spoolerthreads "Number of spooler threads"
  nsv_set _ns_config help.ns/server/$server/module/nssock.writerthreads "Number of writer threads"
  nsv_set _ns_config help.ns/server/$server/module/nssock.writersize "Min return file size when to use writer"
  nsv_set _ns_config help.ns/server/$server/module/nssock.writerbufsize "Size of the send buffer for writer threads"
  nsv_set _ns_config help.ns/server/$server/module/nssock.readtimeoutlogging "Log timed-out waiting for complete request"
  nsv_set _ns_config help.ns/server/$server/module/nssock.serverrejectlogging "Log unable to match request to a virtual server"
  nsv_set _ns_config help.ns/server/$server/module/nssock.sockerrorlogging "Log malformed request, or would exceed request limits"
  nsv_set _ns_config help.ns/server/$server/module/nssock.sockshuterrorlogging "Log error while attempting to shutdown a socket during connection close"
  nsv_set _ns_config help.ns/server/$server/module/nssock.maxheaders "Max size of HTTP headers"
  nsv_set _ns_config help.ns/server/$server/module/nssock.sndbuf "Size of TCP send buffer"
  nsv_set _ns_config help.ns/server/$server/module/nssock.maxqueuesize "Max size of sockets in the queue"
  nsv_set _ns_config help.ns/server/$server/module/nssock.rcvbuf "Size of TCP receive buffer"
  nsv_set _ns_config help.ns/server/$server/module/nssock.sendwait "Timeout for sending data"
  nsv_set _ns_config help.ns/server/$server/module/nssock.recvwait "Timeout for receiving data"
  nsv_set _ns_config help.ns/server/$server/module/nssock.closewait "Timeout for closing socket"
  nsv_set _ns_config help.ns/server/$server/module/nssock.keepwait "Timeout for keep-alive requests"
  nsv_set _ns_config help.ns/server/$server/module/nssock.backlog "Number of sockets in the TCP listen queue"
  nsv_set _ns_config help.ns/server/$server/module/nssock.keepallmethods "Keepalive all methods or just GET"
  nsv_set _ns_config help.ns/server/$server/module/nssock.location "Full driver location"

  nsv_set _ns_config help.ns/server/$server/module/nslog.file "Path to the access log file"
  nsv_set _ns_config help.ns/server/$server/module/nslog.formattedtime "If true then use common log format"
  nsv_set _ns_config help.ns/server/$server/module/nslog.logcombined "If true then use NCSA combined format"
  nsv_set _ns_config help.ns/server/$server/module/nslog.logreqtime "Put in the log request elapsed time"
  nsv_set _ns_config help.ns/server/$server/module/nslog.maxbuffer "Max # of lines in the buffer, 0 ni limit"
  nsv_set _ns_config help.ns/server/$server/module/nslog.maxbackup "Max # of files to keep when rolling"
  nsv_set _ns_config help.ns/server/$server/module/nslog.rollhour "Time to roll log"
  nsv_set _ns_config help.ns/server/$server/module/nslog.rolllog "If true then do the log rolling"
  nsv_set _ns_config help.ns/server/$server/module/nslog.rollonsignal "If true then roll the log on SIGHUP"
  nsv_set _ns_config help.ns/server/$server/module/nslog.suppressquery "If true then don't show query string in the log"
  nsv_set _ns_config help.ns/server/$server/module/nslog.checkforproxy "If true ten check for X-Forwarded-For header"
  nsv_set _ns_config help.ns/server/$server/module/nslog.extendedheaders "List of additional headers to put in the log"

  nsv_set _ns_config help.ns/server/$server/module/nscp.address "IP Address to listen on"
  nsv_set _ns_config help.ns/server/$server/module/nscp.port "TCP port for telnet access"
  nsv_set _ns_config help.ns/server/$server/module/nscp.echopassword "Enable echoing password"
  nsv_set _ns_config help.ns/server/$server/module/nscp.cpcmdlogging "Enable command logging"

  nsv_set _ns_config help.ns/server/$server/module/vhost.enabled "enable/disable virtual hosting"
  nsv_set _ns_config help.ns/server/$server/module/vhost.hostprefix "Prefix between serverdir and host name"
  nsv_set _ns_config help.ns/server/$server/module/vhost.stripport "Remove :port in the Host: header when building pageroot path so Host: www.host.com:80 will result in pageroot serverdir/www.host.com"
  nsv_set _ns_config help.ns/server/$server/module/vhost.stripwww "Remove www. prefix from Host: header when building pageroot path so Host: www.host.com will result in pageroot serverdir/host.com"
  nsv_set _ns_config help.ns/server/$server/module/vhost.hosthashlevel "Hash the leading characters of string into a path, skipping periods and slashes. For example, given the string 'foo' and the levels 2, 3: foo, 2 -> /f/o"

  nsv_set _ns_config help.ns/server/$server/module/nsproxy.exec "Proxy program to start"
  nsv_set _ns_config help.ns/server/$server/module/nsproxy.evaltimeout "# Timeout (ms) when evaluating scripts"
  nsv_set _ns_config help.ns/server/$server/module/nsproxy.gettimeout "# Timeout (ms) when getting proxy handles"
  nsv_set _ns_config help.ns/server/$server/module/nsproxy.sendtimeout "# Timeout (ms) to send data"
  nsv_set _ns_config help.ns/server/$server/module/nsproxy.recvtimeout "# Timeout (ms) to receive results"
  nsv_set _ns_config help.ns/server/$server/module/nsproxy.waittimeout "# Timeout (ms) to wait for slaveis to die"
  nsv_set _ns_config help.ns/server/$server/module/nsproxy.maxslaves "# Max number of allowed proxies slaves alive"
  nsv_set _ns_config help.ns/server/$server/module/nsproxy.minslaves "# Min number of proxy slaves alive"

  nsv_set _ns_config help.ns/server/$server.enabletclpages "Parse *.tcl files in pageroot if enabled"
  nsv_set _ns_config help.ns/server/$server.connsperthread "Normally there's one conn per thread"
  nsv_set _ns_config help.ns/server/$server.flushcontent "Flush all data before returning"
  nsv_set _ns_config help.ns/server/$server.maxconnections "Max connections to put on queue"
  nsv_set _ns_config help.ns/server/$server.maxthreads "Tune this to scale your server of max number of running threads"
  nsv_set _ns_config help.ns/server/$server.minthreads "Min number of idle threads"
  nsv_set _ns_config help.ns/server/$server.threadtimeout "Idle threads die at this rate"
  nsv_set _ns_config help.ns/server/$server.checkmodifiedsince "Honor If-Modifified-Since requests"
  nsv_set _ns_config help.ns/server/$server.preserve "Preserve header case"
  nsv_set _ns_config help.ns/server/$server.urlstats "Enable/disable url statistics"
  nsv_set _ns_config help.ns/server/$server.realm "Ralm to use in Basic Authentiction"
  nsv_set _ns_config help.ns/server/$server.noticedetail "Append server/host info in notice responses"
  nsv_set _ns_config help.ns/server/$server.errorminsize "Min size of error messages, covers MSIE bug"

  nsv_set _ns_config help.ns/server/stats.enabled "Enable/disable statistics web page"
  nsv_set _ns_config help.ns/server/stats.url "Url where to access statistics webpage"
  nsv_set _ns_config help.ns/server/stats.user "User name for access"
  nsv_set _ns_config help.ns/server/stats.password "Password for access"

  nsv_set _ns_config help.ns/server/nsconfig.enabled "Enable/disable config web page"
  nsv_set _ns_config help.ns/server/nsconfig.url "Url where to access config webpage"
  nsv_set _ns_config help.ns/server/nsconfig.user "User name for access, if empty no basic authentication will be used"
  nsv_set _ns_config help.ns/server/nsconfig.password "Password for access"

  nsv_set _ns_config help.ns/server/$server/adp.map "Extensions to parse as ADP's"
  nsv_set _ns_config help.ns/server/$server/adp.enableexpire "Set 'Expires: now' on all ADP's"
  nsv_set _ns_config help.ns/server/$server/adp.enabledebug "Allow Tclpro debugging with ?debug."
  nsv_set _ns_config help.ns/server/$server/adp.startpage "ADP start page to use for empty ADP requests"
  nsv_set _ns_config help.ns/server/$server/adp.errorpage "ADP error page"
  nsv_set _ns_config help.ns/server/$server/adp.defaultparser "Default parser to use for parsing ADP pages"
  nsv_set _ns_config help.ns/server/$server/adp.cachesize "Cache size for ADP compiled pages"

  nsv_set _ns_config help.ns/server/$server/adp/compress.enable "Enable/disable on-the-fly compression"
  nsv_set _ns_config help.ns/server/$server/adp/compress.level "Compression level"
  nsv_set _ns_config help.ns/server/$server/adp/compress.minsize "Min size fo the file for compression"

  nsv_set _ns_config help.ns/server/$server/fastpath.serverdir "Defines absolute path to server's home directory"
  nsv_set _ns_config help.ns/server/$server/fastpath.pagedir "Defines absolute or relative to serverdir directory where all html/adp pages are located"
  nsv_set _ns_config help.ns/server/$server/fastpath.cache "Enable cache for normal URLs. Optional, default is false"
  nsv_set _ns_config help.ns/server/$server/fastpath.cachemaxsize "Size of fast path cache. Optional, default is 5120000"
  nsv_set _ns_config help.ns/server/$server/fastpath.cachemaxentry "Largest file size allowed in cache. Optional, default is cachemaxsize / 10"
  nsv_set _ns_config help.ns/server/$server/fastpath.mmap "Use mmap() for cache. Optional, default is false"
  nsv_set _ns_config help.ns/server/$server/fastpath.directoryfile "List of directory index/default page to look for"
  nsv_set _ns_config help.ns/server/$server/fastpath.directorylisting "Directory listing style. Optional, Can be fancy or simple"
  nsv_set _ns_config help.ns/server/$server/fastpath.directoryproc "Name of Tcl proc to use to display directory listings. Optional, default is to use _ns_dirlist. You can either specify directoryproc, or directoryadp - not both"
  nsv_set _ns_config help.ns/server/$server/fastpath.directoryadp "Name of ADP page to use to display directory listings. Optional. You can either specify directoryadp or directoryproc - not both"

  nsv_set _ns_config help.ns/parameters.dbcloseonexit "Close DB handles on server exit if driver needs graceful shutdown"
  nsv_set _ns_config help.ns/parameters.tclinitlock "Enable/disable Tcl locking on init"
  nsv_set _ns_config help.ns/parameters.tcllibrary "Where all shared Tcl modules are located"
  nsv_set _ns_config help.ns/parameters.home "Home directory where server is installed"
  nsv_set _ns_config help.ns/parameters.debug "Enable debugging log enteies"
  nsv_set _ns_config help.ns/parameters.serverlog "Path to the main server log file"
  nsv_set _ns_config help.ns/parameters.pidfile "Path to the server pid file"
  nsv_set _ns_config help.ns/parameters.hackcontenttype "Automatic adjustment of response content-type header to include charset"
  nsv_set _ns_config help.ns/parameters.outputcharset "Default output charset.  When none specified, no character encoding of output is performed"
  nsv_set _ns_config help.ns/parameters.urlcharset "Default Charset for Url Encode/Decode. When none specified, no character set encoding is performed"
  nsv_set _ns_config help.ns/parameters.preferredcharsets "This parameter supports output encoding arbitration"
  nsv_set _ns_config help.ns/parameters.logroll "Enable/disable log file rolling"
  nsv_set _ns_config help.ns/parameters.logdebug "Log debugginf messages"
  nsv_set _ns_config help.ns/parameters.logdev "Log development messages"
  nsv_set _ns_config help.ns/parameters.lognotice "Log notice messages"
  nsv_set _ns_config help.ns/parameters.logusec "Logging usec as well"
  nsv_set _ns_config help.ns/parameters.logexpanded "Expanded log with extra newlines"
  nsv_set _ns_config help.ns/parameters.logmaxbackup "Max number of backup files"
  nsv_set _ns_config help.ns/parameters.logmaxlevel "Max severity level for logging"
  nsv_set _ns_config help.ns/parameters.logmaxbuffer ""
  nsv_set _ns_config help.ns/parameters.smtphost "Host of the SMTP server, localhost is default"
  nsv_set _ns_config help.ns/parameters.smtptimeout "Timeout for SMTP connections"
  nsv_set _ns_config help.ns/parameters.user "User name the server is running as"
  nsv_set _ns_config help.ns/parameters.group "Group name the server is running as"
  nsv_set _ns_config help.ns/parameters.shutdowntimeout "Timeout for waiting threads to shutdown"
  nsv_set _ns_config help.ns/parameters.schedmaxelapsed "Max time for scheduled procs to run in non-threaded mode"
  nsv_set _ns_config help.ns/parameters.listenbacklog "Number of sockets to use in listen call"
  nsv_set _ns_config help.ns/parameters.dnscache "Enable/disable dns caching"
  nsv_set _ns_config help.ns/parameters.dnscachemaxsize "Max size of dns cache"
  nsv_set _ns_config help.ns/parameters.dnswaittimeout "Timeout for dns resolver"
  nsv_set _ns_config help.ns/parameters.dnscachetimeout "Timeout for dns cache updates"
}

proc _ns_config.header { {title "Section List"} } {

    set html "
      <html>
      <body bgcolor=#ffffff>
      <head>
      <title>Naviserver Config: [ns_info hostname]</title>
      <style>
        body    { font-family: verdana,arial,helvetica,sans-serif; font-size: 8pt; color: #000000; background-color: #ffffff; }
        td      { font-family: verdana,arial,helvetica,sans-serif; font-size: 8pt; }
        pre     { font-family: courier new, courier; font-size: 10pt; }
        h3      { color: #666699; }
        a       { color: blue; font-weight:bold; font-size: 10pt; }
        input   { font-family: verdana,helvetica,arial,sans-serif; font-size: 10pt; background-color: #eaeaea; }
        .lbl    { font-weight: bold; font-size: 10pt; text-align:right;}
      </style>
      </head>

      <table border=0 cellpadding=5 cellspacing=0 width=\"100%\" bgcolor=#a7a7d2>
      <tr>
        <td style=\"color:#ffcc00; font-weight:bold;\">
          Naviserver Config:
        </td>
        <td>
          <a href=?@page=index style=\"color: #000000;\" title=\"List of all sections\" >Section List</a> |
          <a href=?@page=expand style=\"color: #000000;\" title=\"All sections with parameters\" >Expand All</a> |
          <a href=?@page=config style=\"color: #000000;\" title=\"Generate nsd.tcl config file\" >Generate Config</a> |
          <a href=?@page=reread style=\"color: #000000;\" title=\"Reload config from the runtime memory\" >Reload Config</a> |
          <a href=?@page=info style=\"color: #000000;\" title=\"Show server info\" >Info</a> |
          <a href=?@page=reboot style=\"color: #000000;\" title=\"Reboot server\" >Reboot Server</a>
        </td>
        <td style=\"color:#ffffff; font-weight:bold;\" align=right>
          [ns_fmttime [ns_time]]
        </td>
      </tr>
      </table>

      <h3>$title</h3>

      <form name=nsconf method=post action=[ns_conn url]>
      <input type=hidden name=\"@section\" value=\"[ns_queryget @section]\">"

    return $html
}

proc _ns_config.footer {} {

    return "</form></body></html>"
}

proc _ns_config.index {} {

    set html [_ns_config.header]
    append html "<ul>"
    foreach section [_ns_config.sectionList] {
      append html "<li> <a href=?@page=section&@section=$section>$section</a><br>"
    }
    append html "</ul>"
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.expand {} {

    set html [_ns_config.header]
    append html "<ul>"
    foreach section [_ns_config.sectionList] {
      append html "<li> <a href=?@page=section&@section=$section>$section</a><br><ul>"
      foreach key [lsort [nsv_array names _ns_config $section|*]] {
        append html "<li>[lindex [split $key |] 1] = [nsv_get _ns_config $key]"
      }
      append html "</ul>"
    }
    append html "</ul>"
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.section {} {

    set section [ns_queryget @section]
    if { $section == "" } {
        return [_ns_config.index]
    }
    set title "Section: $section:"

    switch -- [ns_queryget @cmd] {
     Save {
        set form [ns_getform]
        set new_name ""
        set new_value ""
        for { set i 0 } { $i < [ns_set size $form] } { incr i } {
          switch -- [set key [ns_set key $form $i]] {
           "" - @cmd - @section {
           }
           @name {
             set new_name [ns_set value $form $i]
           }
           @value {
             set new_value [ns_set value $form $i]
           }
           default {
             nsv_set _ns_config $section|$key [ns_set value $form $i]
           }
          }
        }
        if { $new_name != "" } {
            nsv_set _ns_config $section|$new_name $new_value
        }
        append title " saved."
     }

     r {
        if { [set key [ns_queryget @key]] != "" } {
            nsv_unset -nocomplain _ns_config $section|$key
            append title " removed."
        }
     }
    }

    # All possible parameters for this section
    foreach param [_ns_config.sectionParams $section] {
      set params($param) 1
    }
    set html [_ns_config.header $title]

    append html "<table border=0 cellpadding=5 cellspacing=5>"
    foreach key [lsort [nsv_array names _ns_config $section|*]] {
      set value [nsv_get _ns_config $key]
      set key [string tolower [lindex [split $key |] 1]]
      if { [info exists params($key)] } {
          unset params($key)
      }
      append html "<tr><td class=lbl>$key:</td>
                       <td><input type=text size=40 name=\"$key\" value=\"$value\">
                           <a href=?@page=section&@cmd=r&@section=$section&@key=$key
                              style=\"font-size:8pt;\"
                              title=\"Remove entry\"
                              onClick=\"return confirm('Remove $key?')\">(X)</a>
                       </td>
                       <td style=\"font-size:7pt;color:gray;\"><i>[_ns_config.sectionHelp $section $key]</i></td>
                   </tr>"
    }
    set selectbox ""
    foreach param [lsort [array names params]] {
      append selectbox "<option>$param"
    }
    if { $selectbox != "" } {
        set selectbox "<select style=\"font-size:7pt;\" onChange=\"this.form.elements\['@name'\].value=this.value\">
                       <option value=''>--$selectbox
                       </select><br>"
    }
    append html "<tr><td colspan=2 style=\"color: #666699; border-top: 1px solid #000000;\">New parameter: $selectbox</td></tr>
                 <tr><td><input type=text size=20 name=@name></td>
                     <td><input type=text size=40 name=@value></td>
                 </tr>
                 </table><hr>
                 <input type=submit name=\"@cmd\" value=Save> &nbsp;
                 <input type=button value=Back onclick=\"window.location='?@page=index'\">"
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.config {} {

    set html [_ns_config.header "Generated nsd.tcl config"]
    append html [_ns_config.footer]
    append html "<a href=?@page=config&@cmd=d title=\"Download generated config as text file\">Download</a> |
                 <a href=?@page=config&@cmd=r title=\"Replace current nsd.tcl with generated config\">Replace</a><hr>"

    set data ""
    set maxsize 0

    # Find out the max length of parameter name in order to align properly
    foreach section [_ns_config.sectionList] {
      foreach key [lsort [nsv_array names _ns_config $section|*]] {
        set size [string length [lindex [split $key |] 1]]
        if { $size > $maxsize } {
            set maxsize $size
        }
      }
    }
    # Generate config file
    foreach section [_ns_config.sectionList] {
      append data "ns_section\t$section\n"
      foreach key [lsort [nsv_array names _ns_config $section|*]] {
        set value [nsv_get _ns_config $key]
        set key [lindex [split $key |] 1]
        if { [string first " " $value] > 0 } {
            set value "\"$value\""
        }
        append data "ns_param\t$key [string repeat " " [expr $maxsize-[string length $key]]] $value\n"
      }
      append data "\n"
    }

    # Execute specific command if any
    switch -- [ns_queryget @cmd] {
     d {
       ns_return 200 text/plain $data
     }

     r {
       if { [catch {
           file copy -force [ns_info config] [ns_info config].bak
           ns_filewrite [ns_info config] $data
       } errmsg] } {
           append data "<font color=red>$errmsg</font>"
       }
     }
    }

    append html "<pre>$data</pre>"
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.info {} {

    global tcl_patchLevel

    set html [_ns_config.header "Naviserver Info"]

    append html "
          <table border=0>
          <tr valign=top><td class=lbl>Server:</td><td>[ns_info name] [ns_info patchlevel] [ns_info platform]</td></tr>
          <tr valign=top><td class=lbl>Host:</td><td>[ns_info hostname]/[ns_info address]</td></tr>
          <tr valign=top><td class=lbl>Listen:</td><td>[ns_config "ns/server/[ns_info server]/module/nssock" address [ns_config "ns/server/[ns_info server]/module/nssock" hostname]]:[ns_config "ns/server/[ns_info server]/module/nssock" port]</td></tr>
          <tr valign=top><td class=lbl>Uptime:</td><td>[_ns_config.fmtSeconds [ns_info uptime]]</td></tr>
          <tr valign=top><td class=lbl>Tcl:</td><td>$tcl_patchLevel</td></tr>
          <tr valign=top><td class=lbl>Home:</td><td>[ns_info home]</td></tr>
          <tr valign=top><td class=lbl>Config:</td><td>[ns_info config]</td></tr>
          <tr valign=top><td class=lbl>Tcl Library Path:</td><td>[ns_config ns/parameters tcllibrary]</td></tr>
          <tr valign=top><td class=lbl>Log:</td><td>[ns_info log]</td></tr>
          <tr valign=top><td class=lbl>Pageroot:</td><td>[ns_info pageroot]</td></tr>
          <tr valign=top><td class=lbl>Filters:</td><td>"

    foreach filter [ns_info filters] {
      append html $filter "<br>"
    }
    append html "</td></tr>
          <tr valign=top><td class=lbl>Procs:</td><td>"

    foreach proc [ns_info requestprocs] {
      append html $proc "<br>"
    }
    append html "</td></tr>
          <tr valign=top><td class=lbl>Traces:</td><td>"

    foreach trace [ns_info traces] {
      append html $trace "<br>"
    }
    append html "</td></tr>
          <tr valign=top><td class=lbl>Callbacks:</td><td>"

    foreach callback [ns_info callbacks] {
      append html $callback "<br>"
    }
    append html "</td></tr>
          </table>"

    append html [_ns_config.footer]

    return $html
}

proc _ns_config.reboot {} {

    set html [_ns_config.header "Reboot the server"]
    if { [ns_queryget reboot] == "" } {
        append html "Click on the button to reboot the server:<p>
                     <input type=button value=Reboot onclick=\"window.location='?@page=reboot&reboot=1'\"> &nbsp;
                     <input type=button value=Back onclick=\"window.location='?@page=index'\">"
    } else {
        append html "Server is being rebooted...
                     <script>setTimeout('window.location=\"?@page=index\"',5000)</script>"
        ns_shutdown 5
    }
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.reread {} {

    foreach section [nsv_array names _ns_config @*] {
      nsv_unset -nocomplain _ns_config $section
      foreach key [nsv_array names _ns_config [string range $section 1 end]|*] {
        nsv_unset -nocomplain _ns_config $key
      }
    }

    set html [_ns_config.header "Config has been reloaded"]
    append html "<script>setTimeout('window.location=\"?@page=index\"',1000)</script>"
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.sectionList {} {

    set sections ""
    foreach section [ns_configsections] {
      set name [ns_set name $section]
      lappend sections $name
      _ns_config.sectionInit $name $section
    }
    return [lsort $sections]
}

proc _ns_config.sectionInit { name section } {

    if { ![nsv_exists _ns_config @$name] } {
        nsv_set _ns_config @$name [ns_set size $section]
        for { set i 0 } { $i < [ns_set size $section] } { incr i } {
          nsv_set _ns_config $name|[ns_set key $section $i] [ns_set value $section $i]
        }
    }
}

proc _ns_config.sectionHelp { section name } {

    set help ""
    foreach key [nsv_array names _ns_config help.$section.*] {
      if { [string match -nocase $key help.$section.$name] } { return [nsv_get _ns_config $key] }
    }
    return $help
}

proc _ns_config.sectionParams { section } {

    set params ""
    foreach key [nsv_array names _ns_config help.$section.*] {
      if { [string index $key end] != "*" } { lappend params [lindex [split $key .] end] }
    }
    return $params
}

proc _ns_config.fmtSeconds { seconds } {

    if { $seconds < 60 } {
        return "${seconds} (s)"
    }

    if { $seconds < 3600 } {
        set mins [expr {$seconds/60}]
        set secs [expr {$seconds - ($mins * 60)}]

        return "${mins}:${secs} (m:s)"
    }

    set hours [expr {$seconds/3600}]
    set mins [expr ($seconds - ($hours * 3600))/60]
    set secs [expr {$seconds - (($hours * 3600) + ($mins * 60))}]

    return "${hours}:${mins}:${secs} (h:m:s)"
}

# Main processing logic
set page [ns_queryget @page]
if { [info command _ns_config.$page] == "" } {
  set page index
}

# Check user access if configured
if { $user != "" && ([ns_conn authuser] != $user || [ns_conn authpassword] != $password) } {
  ns_returnunauthorized
  return
}
# Produce page
ns_set update [ns_conn outputheaders] "Expires" "now"
ns_return 200 text/html [_ns_config.$page]
