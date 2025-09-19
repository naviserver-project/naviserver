<%
  ;# Welcome to the upper terrority of an *.adp file,
  ;#  As you scroll through this page looking in confusion, you may find it intriguring
  ;#  Take a breath and start from the top
  ;#    The first procedure is commented, see if you can tag along :-)
  ;#
  ;#  try { }
  ;#    This is wrapped around our procedure below
  ;#      this allows us to capture any errors
  ;#      or store the error in a specifc variable
  ;#      allowing us to hanlde the error without interuptting the user
  ;#
  ;#  proc probe_driver_info {opt} {}
  ;#    proc is short for procedure
  ;#      when called it executes code within it's body
  ;#      we can pass information in to the procedure
  ;#      in this case we store this information in a variable called "opt"
  ;#      short-hand for option

try {
  proc probe_driver_info {opt} {
    ;# we've now created our first procedure, well done.

    ;# We construct a second procedure for simple debugging
    ;#  This procedure has two variables: "enable" and "input"
    ;#  enable - allows us if we wish to display the debug out or not
    ;#  input  - displays the data passed to the procedure
    proc debug {enable input} { switch $enable { 1 { puts ">>> $input <<<" } } }
    ;# Examples
    debug 0 [ns_driver info]  ;# Display the raw output of NaviServer's internal information
    debug 0 [concat {*}[ns_driver info]]  ;# The above output is very long
                                          ;# Concat collapses the data in from a long string of information in to a list
                                          ;# this list enables us later on to pick and choose which value we desire
                                          ;# Enable to see the results :)

    ;# Hold our driver information in to a list
    variable nssock_driver_info [concat {*}[ns_driver info]]

    ;# Our variables to hold our bound addresses and ports
    variable bound_to           [dict get $nssock_driver_info address]
    variable at_port            [dict get $nssock_driver_info port]

    ;# Navi Instance Version
    variable navi_version       [ns_info patchlevel]

    ;# Operating System & Version
    variable operating_system         $::tcl_platform(os)
    variable operating_system_version $::tcl_platform(osVersion)

    ;# Lets begin this adventure with our first switch
    ;#  Switch: $opt       - What option did we decide to call
    ;#    Case: address      - Execute the code within our address case       - Displays the address(es) from our driver information variable
    ;#    Case: modules      - Execute the code within our modules case       - Displays the loaded modules
    ;#    Case: navi_version - Execute the code within our navi_version case  - Displays NaviServer's version
    ;#    Case: port         - Execute the code within our port case          - Displays the bound ports that NaviServer sits upon
    ;#    Case: protocol     - Execute the code within our protocol case      - Displays the working protocol
    ;#    Case: os           - Execute the code within our os case            - Displays our Operating System

    switch $opt {
      address { ;# We need to differate between "Address" and "Addresses" in case of when the Navi instance is bound to one or more IP
        ;#  Switch: [llength $bound_to]                                      - Retrieve the bound address(es) from the server configuration
        ;#                                                                        check to see if we have multiple bound addresses
        ;#  Case: 1      - We only have one entry for the bound address       - Display "Listen on Address"
        ;#  Case default - We have two or more bound addresses                - Display "Listen on Addresss"
        switch [llength $bound_to] {
          1       {
            return \
              "<code id=\"terminal\">Listening on address: $bound_to</code> \
              at port <code class=\"terminal\"> $at_port</code>"
          }
          default {
            return \
              "<code id=\"terminal\">Listening on addresses:</code> \
              <code id=\"general\">$bound_to</code> \
              at ports <code id=\"general\"> $at_port</code>"
          }
        } ;# end switch llength
      }

      modules {    ;# Show me the shades, return our modules
        variable modules [ns_ictl getmodules]
        variable modules [append modules "[nsv_dict get system modules_enabled live]"]
        #
        return \
          "<code id=\"terminal\">Modules Available:</code> <code id=\"general\">$modules</code>"
      }

      navi_version { ;# Hey it's me Navi, return our version number
        return \
          "<code id=\"terminal\">NaviServer Version:</code> <code id=\"general\">$navi_version</code>"
      }

      port {        ;# What ports are we bound on?
        switch [llength [dict get $nssock_driver_info port]] {
          1       { return "<code id=\"terminal\">Listening at port: <code id=\"general\">$at_port</code>" }
          default { return "<code id=\"terminal\">Listening at ports: <code id=\"general\">$at_port</code>" }
        } ;# end switch
      }

      protocol { ;# And our protocol is?
        return \
          "<code><strong>protocol:</strong> [dict get $nssock_driver_info protocol]"
      }

      os { ;# Finally tell Tcl to tell us our OS Version
        return \
          "<code id=\"terminal\">Operating System:</code> <code>$operating_system $operating_system_version</code>"
      }
    } ;# end switch option
  } ;# end proc
} ;# end try

proc probe_nsstats {} {
  try {
    nsv_dict set nsstats text not_installed {Module nsstats was not detected <a href="extras/?package=nsstats" class="btn-action">Install It Now</a>}
    nsv_dict set nsstats text installed {Explore the <a href="/nsstats.tcl"> the Navi Statistics Module</a> for real-time performance insights and logging analysis }
    ;# Check to see if our file exists
    variable ns_stats_existence [file exists [ns_server pagedir]/nsstats.tcl]

    switch $ns_stats_existence {
      1 { append modules "\u00A0" ;# append in unicode a whitespace to the front of our string
          append modules "nsstats"
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
    nsv_dict set security secure page {Please replace or secure this default page to protect your server credentials.}

    variable display_mode [lindex $args 0] ;# Display mode passed to the procedure
    switch $display_mode {
        css {
          return [read [open "/forest/fox/~navi/pages/css/default.css" r]]
        }
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
        ###
        return [ns_trim -delimiter | [subst {
        |  <meta charset="utf-8" name="viewport" content="width=device-width, initial-scale=1.0">
        |   <title>NaviServer – Welcome</title>
        |   <link rel="stylesheet" href="doc/naviserver/man-5.0.css" type="text/css">
        |   <link rel="icon" type="image/svg+xml" href="favicon.svg">
        }]
        ]
        ###
      }
    } ;# end switch
  } ;# end try
  } ;# end proc

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
<!--  Rendering the above below --!>
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
      <li><%=[probe_driver_info os]%></li>
      <li><%=[probe_driver_info navi_version]%></li>
      <li><%=[probe_driver_info address]%></li>
      <li><%=[probe_driver_info modules]%></li>
    </div>
      <%=[display security_advice]%>
      <%=[display password_unchanged]%>
  </div>
</body>
</html>
