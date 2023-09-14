<!DOCTYPE html>
<html lang='en'>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title><%= [ns_quotehtml $title] %></title>
</head>
<body>
  <h2><%= [ns_quotehtml $title] %></h2>
  <%= $notice %>
  <% if {[info exists noticedetail] && $noticedetail} {
     ns_adp_puts [subst [ns_trim -delimiter | {
     |<p style='text-align: right; font-size: small; font-style: italic;'>
     |  [ns_info name]/[ns_info patchlevel]
     |</p>
     }]]
     }
   %>
</body>
