
<%
## $Header$

upvar one   eight
upvar two   nine
upvar three ten

set one   "unchanged1"
set two   "unchanged2"
set three "unchanged3"
%>

<b>
<%=$eight%>, <%=$nine%>, <%=$ten%>
</b>

<br>

<b>
<%=$one%>, <%=$two%>, <%=$three%>
</b>

<br>

<%
set ten "blah"
%>

<b>
<%=$eight%>, <%=$nine%>, <%=$ten%>
</b>

<br>

<b>
<%=$one%>, <%=$two%>, <%=$three%>
</b>

