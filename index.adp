<!--
    ;# Welcome to the upper terrority of an *.adp file,
    ;#  As you scroll through this page looking in confusion, you may find it intriguring
    ;#  Take a breath and start from the top
    ;#    The first procedure is commented, see if you can tag along :-)
--!>
<%
  ;#  We wish to safeguard this procedure, using try enables us to catch any errors
  ;#  the command "proc" constructs a foundation for the procedure named probe_driver_info
  ;#    the { } defines our information gates
  ;##   This is where information can be passed within
  ;#      In this case, we construct the gate "opt" short-hand for option, for our option menu below
try { proc probe_driver_info {opt} {
    ;# we've now created our procedure, well done.
    ;# the pillars are up but we have no working sand
    ;# so let's start

      ;# This procedure enables us to evalulate an output of a variable
        proc debug {enable input} { switch $enable { 1 { puts ">>> $input <<<" } } }
        debug 0 [ns_driver info]  ;# raw output of the internals information
        debug 0 [concat {*}[ns_driver info]] ;# we convert our long string of information in to a list
                                             ;# the list enables us later on to pick and choose which value we desire
        variable nssock_driver_info [concat {*}[ns_driver info]] ;# all our information we require is currently in one very long string without a fullstop
        ;# and so lets begin this adventure
        switch $opt {
          protocol { ;# Was the protocol passed to the gate?
            return "<code><strong>protocol:</strong> [dict get $nssock_driver_info protocol]"
          }
          address { ;# Or maybe it was a address request
            ;#  This switch determines if the server is hosted on two IP addresses
            ;#    A cute script to
            switch [llength [dict get $nssock_driver_info address]] {
              1       { return "<code id=\"terminal\">Listening on address:[dict get $nssock_driver_info address]</code>\
                                at port <code class=\"terminal\"> [dict get $nssock_driver_info port]</code>"  }
              default { return "<code id=\"terminal\">Listening on addresses:</code> <code id=\"general\">[dict get $nssock_driver_info address]</code> \
                                at ports <code id=\"general\"> [dict get $nssock_driver_info port]</code>" }
            }
          }
          port { ;# Was it a port?
            switch [llength [dict get $nssock_driver_info address]] {
              1       { return "<code id=\"terminal\">Listening at port: <code id=\"general\">[dict get $nssock_driver_info port]</code>" }
              default { return "<code id=\"terminal\">Listening at ports: <code id=\"general\">[dict get $nssock_driver_info port]</code>" }
            } ;# end switch
          }
          os { ;# maybe an os?
            return "<code id=\"terminal\">Operating System:</code> <code>$::tcl_platform(os) $::tcl_platform(osVersion)</code>"
          }
          modules { ;# show me the shades
            variable modules [ns_ictl getmodules]
            variable modules [append modules "[nsv_dict get system modules_enabled live]"]

            return "<code id=\"terminal\">Modules Available:</code> <code id=\"general\">$modules</code>"
          }
          navi_version { ;# Your navi's version code
            return "<code id=\"terminal\">NaviServer Version:</code> <code id=\"general\">[ns_info patchlevel]</code>"
          }
        } ;# end switch option
      } ;# end proc
} ;# end try

proc probe_nsstats {} {
  try {
    ;# Check to see if our file exists
    variable ns_stats_existence [file exists [ns_server pagedir]/nsstats.tcl]
    #
    nsv_dict set nsstats text not_installed {Module nsstats was not detected <a href="extras/?package=nsstats" class="btn-action">Install It Now</a>}
    nsv_dict set nsstats text installed {Explore the <a href="/nsstats.tcl"> the Navi Statistics Module</a> for real-time performance insights and logging analysis }
    #
  ;# return the following dictonary
    switch $ns_stats_existence {
      1 { append modules "\u00A0" ;# append a space to the front of our string
          append modules "nsstats"
          ;# include a front facing whitespace
          nsv_dict set system modules_enabled live "$modules"
          return [nsv_dict get nsstats text installed] }
      0 { return [nsv_dict get nsstats text not_installed] }
    } ;# end switch
  } ;# end try
} ;# end proc

proc display { args } {
  try {
    nsv_dict set image icon alert { &#9888; }
    nsv_dict set btn_action nsperm passwd_msg {The default system administrator password remains unchanged}
    nsv_dict set btn_action nsperm passwd_resolve {Change System Password Now}

    variable display_mode [lindex $args 0] ;# Display mode passed to the procedure
    switch $display_mode {
        css { return [read [open "/css/default.css" r]] }
        welcome {
        ###
        return [ns_trim -delimiter | {
        |<div>
        | <h1>Welcome to your NaviServer</h1>
        | <p>
        |   Congratulations – your navi instance is up and running!
        |   <br>
        |   This page confirms that the default installation is active.
        | </p>
        |</div>
          }]
        ###
      }
      security_advice {
        nsv_dict set security secure page {Please replace or secure this default page to protect your server credentials.}
        ###
        return [ns_trim -delimiter | [subst {
        |<div class="security">
        |    <table>
        |        <tr>
        |            <td class="icon_cell">
        |            [nsv_dict get image icon alert]
        |            </td>
        |            <td>
        |            [nsv_dict get security secure page]
        |            </td>
        |    </table>
        |</div>
          }]
        ]
        ###
      }
      password_unchanged {
      ###
      return [ns_trim -delimiter | [subst {
      |<div class="security">
      |    <table>
      |        <tr>
      |            <td class="icon_cell">
      |            [nsv_dict get image icon alert]
      |            </td>
      |            <td>
      |            [nsv_dict get btn_action nsperm passwd_msg]
      |            </td>
      |        </tr>
      |        <tr>
      |            <td class="icon_cell"></td> <!-- Empty icon cell for the second row -->
      |            <td class="right_align">
      |                <button id="changePwdBtn" class="btn_action">[nsv_dict get btn_action nsperm passwd_resolve]</button>
      |            </td>
      |        </tr>
      |    </table>
      |</div>
        }]
        ]
      ###
    }
    header {
    ###
    return [ns_trim -delimiter | [subst {
    |<a href="http://127.0.0.1:8080/"><strong>NaviServer</strong></a>
    |<span class="tagline">Programmable Web Server</span>
    }]
    ]
    ###
    }
    meta {
      return [ns_trim -delimiter | [subst {
      |  <meta charset="utf-8" name="viewport" content="width=device-width, initial-scale=1.0">
      |   <title>NaviServer – Welcome</title>
      |   <link rel="stylesheet" href="doc/naviserver/man-5.0.css" type="text/css">
      |   <link rel="icon" type="image/svg+xml" href="favicon.svg">
      }]
      ]
    }
      connection_info_address { return [probe_driver_info address] }
      connection_info_port    { return [probe_driver_info port] }
      enabled_modules         { return [probe_driver_info modules] }
      os_version              { return [probe_driver_info os] }
      navi_version            { return [probe_driver_info navi_version] }
      } ;# end switch
    } ;# end try
  } ;# end proc

proc local_port {port} {
  variable port [probe_driver_info port]
}

proc show_howto {opt} {
  switch $opt {
    documentation {
      nsv_dict set show_howto documentation general "Review the <a href=\"doc/toc.html\">documentation</a> to understand complete setup instructions and feature details"
      return [nsv_dict get show_howto documentation general]
    }
    replace {
      nsv_dict set show_howto documentation replace_placeholder "Replace this placeholder page with your custom content by configuring the appropriate directory"
      return [nsv_dict get show_howto documentation replace_placeholder]
    }
    nsstats {
      return [probe_nsstats]
    }
  }
  } ;# end proc
%>

<!--
;#  This is the part where we cast the magic
--!>

<!DOCTYPE html>
<html lang="en">
<head>
  <%=[display meta]%>
<style>
  <%=[display css]%>
</style>
</head>

<body>
  <header class="main_header_blue">
    <%=[display header]%>
  </header>

  <div class="display">
    <%=[display welcome]%>

    <h2>How To</h2>
    <ul>
      <li><%=[show_howto replace]%></li>
      <li><%=[show_howto documentation]%></li>
      <li><%=[show_howto nsstats]%></li>
    </ul>

    <h2>Current Server Configuration</h2>
    <div class="code">
      <li><%=[display os_version]%></li>
      <li><%=[display navi_version]%></li>
      <li><%=[display connection_info_address]%></li>
      <li><%=[display enabled_modules]%></li>
    </div>

      <%=[display security_advice]%>
      <%=[display password_unchanged]%>
  </div>
</body>
</html>
