<html>
<head>
  <link rel="stylesheet" href="doc/naviserver/man.css" type="text/css">
  <title>NaviServer <%=[ns_info patchlevel]%></title>
  <style>
   strong {color: #000080;}
   #man-header strong {color: #ffffff;}
  </style>
</head>
<body>
 <div id="man-header">
  <a href="http://wiki.tcl.tk/2090"><span class="logo"></span><strong>NaviServer</strong></a>
  - programmable web server
 </div>

  <h1>
   Welcome to <a href="http://naviserver.sourceforge.net/">NaviServer
   <%=[ns_info patchlevel]%></a> under
   <%=[set . "$::tcl_platform(os) $::tcl_platform(osVersion)"]%>
  </h1>
  <p>
  If you can see this page, then the <a
  href="http://naviserver.sourceforge.net/">NaviServer</a> web server
  was activated on this machine.
  The server installation contains currently just the default content provided by
  the standard NaviServer distribution. In a next step, this placeholder page should be
  replaced, or the configuration should point to a directory with real
  content. The current configuration file is <i><%=[ns_info config]%></i>.
  </p>
  <hr>
  <p>
  <ul>
  <li>The NaviServer <a href="doc/toc.html">Documentation<a> has been included with this distribution.<p>

  <li>The NaviServer <a href="examples/">Examples<a> include a few useful scripts and tricks.<p>

  <%
   ns_adp_puts {<li>The NaviServer <a href="nsstats.tcl">Statistics page</a> can be
                    useful in resolving performance issues.}

   if { ![file exists [ns_server pagedir]/nsstats.tcl] } {
     ns_adp_puts [subst {<br><i>Currently nsstats is not installed as
                     [ns_server pagedir]/nsstats.tcl, to install you need to
                     download modules and do make install in nsstats
                     directory. <a href = 'bitbucket-install.tcl?file=nsstats.tcl'>Install now</a>.
                     </i>}]
   }
  %>
    <p>

  <li>The NaviServer runtime <a href="nsconf.tcl">Config page</a> can be
    useful in reviewing server's setup.<br>
  <%
   if { ![file exists [ns_server pagedir]/nsconf.tcl] } {
     ns_adp_puts [subst {<i>Currently nsconf is not installed yet.
	<a href = 'bitbucket-install.tcl?file=nsconf.tcl'>Install now</a>.</i>
    }]
   } else {
    ns_adp_puts [subst {The nsconf module has to be enabled and protected by a password in
    <strong>[ns_server pagedir]/nsconf.tcl</strong>.}]
    }
    %>
    <p>
  </ul>
  <hr>
 </body>
</html>



