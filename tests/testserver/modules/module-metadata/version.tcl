# Data only: init.tcl registers this metadata before the ordinary library pass.
namespace eval ::module_metadata {}
set ::module_metadata::version 1.2.3
set ::module_metadata::tag test-[ns_info server]
