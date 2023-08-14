# -*- Tcl -*-
puts "Content-type: text/html"
puts ""
puts "<head><title>Hello</title></head>"
puts "<body>"
puts "You are using $::env(HTTP_USER_AGENT)."
puts "</body>"