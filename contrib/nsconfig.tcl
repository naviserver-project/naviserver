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
#   Set of procedures implementing the /_nsconfig, a simple nsd.tcl configurator
#   which takes runtime config and allows to modify and re-create new nsd.tcl file or
#   even replace existing one if priviliges allow.
#
#   To use it, just drop it into tcl/ or modules/tcl directory under
#   naviserver install home which is usually /usr/local/ns and
#   add the following entries into config file (it shows default values).
#
#  ns_section      ns/server/nsconfig
#  ns_param        enabled                 1
#  ns_param        url                     /_nsconfig
#  ns_param        user                    naviserver
#  ns_param        password                admin
#
#   Then restart the server and point the browser to http://localhost/_nsconfig
#

set path "ns/server/nsconfig"

nsv_set _ns_config ns:enabled       [set enabled [ns_config -bool $path enabled 0]]
nsv_set _ns_config ns:url           [set url [ns_config $path url "/_nsconfig"]]
nsv_set _ns_config ns:user          [ns_config $path user "naviserver"]
nsv_set _ns_config ns:password      [ns_config $path password "admin"]

if { $enabled }  {
    ns_register_proc GET $url/* _ns_config.handleUrl
    ns_register_proc POST $url/* _ns_config.handleUrl
    ns_log notice "config: web configuration enabled for '$url'"
}

proc _ns_config.handleUrl {} {

    set page [file rootname [ns_conn urlv [expr {[ns_conn urlc] - 1}]]]

    if { [info command _ns_config.$page] == "" } {
      set page index
    }
    set user [nsv_get _ns_config ns:user]
    set password [nsv_get _ns_config ns:password]

    if { $user != "" && ([ns_conn authuser] != $user || [ns_conn authpassword] != $password) } {
        ns_returnunauthorized
        return
    }

    ns_set update [ns_conn outputheaders] "Expires" "now"
    ns_return 200 text/html [_ns_config.$page]
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
          <a href=index.adp style=\"color: #000000;\" title=\"List of all sections\" >Section List</a> |
          <a href=expand.adp style=\"color: #000000;\" title=\"All sections with parameters\" >Expand All</a> |
          <a href=config.adp style=\"color: #000000;\" title=\"Generate nsd.tcl config file\" >Generate Config</a> |
          <a href=reread.adp style=\"color: #000000;\" title=\"Reload config from the runtime memory\" >Reload Config</a> |
          <a href=info.adp style=\"color: #000000;\" title=\"Show server info\" >Info</a> |
          <a href=reboot.adp style=\"color: #000000;\" title=\"Reboot server\" >Reboot Server</a>
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
      append html "<li> <a href=section.adp?@section=$section>$section</a><br>"
    }
    append html "</ul>"
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.expand {} {

    set html [_ns_config.header]
    append html "<ul>"
    foreach section [_ns_config.sectionList] {
      append html "<li> <a href=section.adp?@section=$section>$section</a><br><ul>"
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

    set html [_ns_config.header $title]

    append html "<table border=0 cellpadding=5 cellspacing=5>"
    foreach key [lsort [nsv_array names _ns_config $section|*]] {
      set value [nsv_get _ns_config $key]
      set key [lindex [split $key |] 1]
      append html "<tr><td class=lbl>$key:</td>
                       <td><input type=text size=40 name=\"$key\" value=\"$value\">
                           <a href=section.adp?@cmd=r&@section=$section&@key=$key
                              style=\"font-size:8pt;\"
                              title=\"Remove entry\"
                              onClick=\"return confirm('Remove $key?')\">(X)</a>
                       </td>
                   </tr>"
    }
    append html "<tr><td colspan=2 style=\"color: #666699; border-top: 1px solid #000000;\">New parameter:</td></tr>
                 <tr><td><input type=text size=10 name=@name></td>
                     <td><input type=text size=40 name=@value></td>
                 </tr>
                 </table><hr>
                 <input type=submit name=\"@cmd\" value=Save> &nbsp;
                 <input type=button value=Back onclick=\"window.location='index.adp'\">"
    append html [_ns_config.footer]

    return $html
}

proc _ns_config.config {} {

    set html [_ns_config.header "Generated nsd.tcl config"]
    append html [_ns_config.footer]
    append html "<a href=config.adp?@cmd=d title=\"Download generated config as text file\">Download</a> |
                 <a href=config.adp?@cmd=r title=\"Replace current nsd.tcl with generated config\">Replace</a><hr>"

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
                     <input type=button value=Reboot onclick=\"window.location='reboot.adp?reboot=1'\"> &nbsp;
                     <input type=button value=Back onclick=\"window.location='index.adp'\">"
    } else {
        append html "Server is being rebooted...
                     <script>setTimeout('window.location=\"index.adp\"',5000)</script>"
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
    append html "<script>setTimeout('window.location=\"index.adp\"',1000)</script>"
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

proc _ns_config.fmtSeconds {seconds} {

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
