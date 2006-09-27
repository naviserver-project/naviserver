<HTML>
 <HEAD>
  <TITLE>Default Page for Naviserver Installation</TITLE>
 </HEAD>
 <BODY
  BGCOLOR="#FFFFFF"
  TEXT="#000000"
  LINK="#0000FF"
  VLINK="#000080"
  ALINK="#FF0000"
 >
  <H1 ALIGN="CENTER">
   Welcome to <A HREF="http://naviserver.sourceforge.net/">Naviserver</A> on
   <%=[string totitle [ns_info platform]]%>
  </H1>
  <P>
  If you can see this page, then the people who own this host have just
  activated the <A HREF="http://naviserver.sourceforge.net/">Naviserver Web server</A>
  software.  They now have to add content to this directory
  and replace this placeholder page, or else point the server at their real
  content.
  </P>
  <HR>
  <P>
  <UL>

  <LI>The Naviserver <A HREF=doc/>Documentation<A> has been included with this distribution.<P>

  <LI>The Naviserver <A HREF=examples/>Examples<A> include useful scripts and tricks.<P>

  <%
   ns_adp_puts {<LI>The Naviserver <A HREF="nsstats.tcl">Statistics page</A> can be
                    useful in resolving performance issues.<P>}

   if { ![file exists [ns_info pageroot]/nsstats.tcl] } {
     ns_adp_puts {<I>Currently it is not installed, to install you need to
                     download modules and do make install in nsstats directory
                  </I>}
   }
  %>

  <%
   ns_adp_puts {<LI>The Naviserver runtime <A HREF="nsconf.tcl">Config page</A> can be
                    useful in reviewing server's setup.<P>}

   if { ![file exists [ns_info pageroot]/nsconf.tcl] } {
     ns_adp_puts {<I>Currently it is not installed, to install you need to
                     download modules and do make install in nsconf directory
                  </I>}
   }
  %>
  </UL>
 </BODY>
</HTML>



