#!/bin/sh
# The next line restarts using tclsh. \
exec tclsh "$0" "$@"

# $Id: win32-install-nsd.tcl,v 1.24 2020/05/05 12:35:49 andy Exp $
# by Andrew Piskorski <atp@piskorski.com>
#
# Simple install script for our NaviServer on Windows.  To run this on
# MS Windows, first install ActiveState ActiveTcl, then from a Command
# Prompt, simply cd to the "naviserver" source directory, and then run
# 'tclsh THIS_SCRIPT'.

# Defaults for command-line arguments, stored in a Tcl array:
set AA(-t) {C:/P/nsd/nsd} ; set AA(-c) 1 ; set AA(-f) [pwd]
set AA(-N) 1 ; set AA(-h) 0 ; set AA(-i) 0
set AA(-m) [list]

proc get_usage_help {argv0 core_dir install_dir module_l} {
   set USAGE "
Recommended Basic Use:
  $argv0 -i 1

Description:
  This script installs NaviServer on Windows by simply copying all the
  appropriate compiled files from the source true to their install
  locations.  All necessary *.h and *.lib files are also included.

  The current copy-from  location is:  $core_dir
  The current install-to location is:  $install_dir
  We will also install these non-core modules:  $module_l

Options:
  All options and arguments to this script must be given in Tcl
  key/value array list style.

  -i 1       : Actually do the install if true.
  -h 1       : Show this help and exit.
  -f PATH    : Copy files from this source directory.
               By default it is taken from your current working directory.
  -t PATH    : Install-to pathname.  May be modified if using '-c 1'.
               You may rename or move it to anything you want after installing.
  -c {0|1}   : Append clock time to install directory name, true or false.
  -m MODULES : Tcl list of non-core modules to install.
"
   return $USAGE
}


proc empty_string_p {str} { return [string equal $str {}] }
proc even_p {int} { return [expr {!($int % 2)}] }

proc get_install_dir {base clock_append_p} {
   if {$clock_append_p} {
      set ff "${base}_[clock format [clock seconds] -format {%Y-%m-%dT%H%M%S}]"
   } else { set ff $base }
   set rr [file normalize $ff]
   if {![check_file_normalize_ok_p $rr]} {
      puts stderr "Warning: get_install_dir: Ignoring bogus 'file normalize' result:  $rr"
      set rr $ff
   }
   return $rr
}

proc get_from_core_dir {from} {
   if {{src} eq $from} {
      # Magic flag value to trigger this different default:
      set from [get_core_dir_via_script_location]
   }
   if {[empty_string_p $from]} { set from [pwd] }
   return [file normalize $from]
}

proc get_core_dir_via_script_location {} {
   # Return full-path to the core "naviserver" source directory based on
   # where we know (assume) THIS script is located in the source tree.

   set my_script [file normalize [info script]]
   if {[empty_string_p $my_script]} {
      set core_dir [pwd]
      puts stderr "Error: 'info script' returned empty string.  Using 'pwd' as core_dir instead:  $core_dir"
   } elseif {0} {
      # Old location for this script in:  ns-fork-pub/mymodule/bin/
      set core_dir "[file dirname [file dirname [file dirname $my_script]]]/naviserver"
   } else {
      # We know (assume) that this script is located in:  naviserver/win32-util/
      set core_dir "[file dirname [file dirname $my_script]]"
   }
   return $core_dir
}

proc check_file_normalize_ok_p {ff {only_unix_p 1}} {
   # If you run on Unix for debugging, [file normalize {C:/foo/bar}] gives
   # weird results, presumably because the path is bogus there.
   if { $only_unix_p } {
      if {{unix} ne $::tcl_platform(platform)} { return 1 }
   }
   set dd [file normalize [pwd]]
   if {[string match "${dd}*" $ff]} { return 0 } else { return 1 }
}


proc copy_catch {from to} {
   # TODO: Some of these copies fail when trying to overwrite existing
   # files.  Looks like the -force has no effect when recursively copying
   # whole directory trees:  --atp@piskorski.com, 2004/06/24 00:58 EDT

   # NOTE: recursively copy/overwrite directory trees. ooa64@ua.fm, 2025/06/26
   if {![file exists "$to"]} {
       file mkdir "$to"
   }
   if { [file isdirectory "$from"] } {
       foreach f [glob -directory $from -nocomplain *] {
           copy_catch $f [file join $to [file tail $from]]
       }
   } elseif { [catch { file copy -force -- "$from" "$to" } errmsg] } {
      if { [string match {*error copying*file already exists*} $errmsg] } {
         puts "Notice: Did NOT overwrite these files:\n  $errmsg"
      } else {
         puts $errmsg
      }
   }
}

proc install_create_subdirs {install_dir navi_p} {
   set create_l [list bin lib include modules servers servers/default/modules/nslog log]
   # TODO: Naviserver's Unix Makefile creates a "logs" directory rather
   # than "log" - do we care?
   if { $navi_p } {
      lappend create_l conf pages
   } else {
      # Old AOLserver only, not NaviServer.
      lappend create_l debug-bin debug-lib
   }

   foreach ff $create_l { file mkdir "${install_dir}/${ff}" }
}


proc install_naviserver_core {core_dir install_dir} {
   # Unfortunately, the current Naviserver Windows nmake build process
   # strews all the generated binaries in the source directories, just
   # like the default Unix build.  --atp@piskorski.com, 2014/10/02 14:58 EDT

   # Make sure install_dir is in Tcl/Unix style 'C:/foo/bar'.  Using
   # Windows-style 'C:\foo\bar' can break stuff here!

   # core_dir should always be the path ending in "naviserver".
   set fl_bin [glob "$core_dir/*/*.exe" "$core_dir/*/*.dll" "$core_dir/nsd/init.tcl"]
   set fl_lib [glob "$core_dir/*/*.lib" "$core_dir/*/*.pdb"]
   set fl_h   [glob "$core_dir/include/*.h" "$core_dir/nsdb/nsdb.h" "$core_dir/include/Makefile.{global,module,win32}"]

   eval file copy -force -- $fl_bin "$install_dir/bin/"
   eval file copy -force -- $fl_h   "$install_dir/include/"
   eval file copy -force -- $fl_lib "$install_dir/lib/"

   # Naviserver this collection of *.tcl files (sendmail.tcl, etc.) into tcl/,
   # while AOLserver used to put them into modules/tcl/:
   set cp_list {
      tcl                      {}
      contrib/examples         pages/
      win32/test/servers/test  servers/
      win32/test/nsd.tcl       servers/test/
      ca-bundle.crt            {}
      tests/testserver/certificates/server.pem certificates/
   }
   foreach ff [list nsd-config.tcl simple-config.tcl openacs-config.tcl sample-config.tcl] {
      lappend cp_list $ff {conf/}
   }
   foreach ff [list index.adp install-from-repository.tcl tests] {
      lappend cp_list $ff {pages/}
   }
   foreach [list from to] $cp_list {
      copy_catch "$core_dir/$from" "$install_dir/$to"
   }

   puts "Naviserver successfully installed into:  $install_dir/"
}


proc get_module_candidate_list {core_dir} {
   set base_dir [file dirname $core_dir]
   return [glob -types d -nocomplain -path $base_dir /*]
}

proc get_module_from_dir {mod core_dir} {
   return [file normalize "${core_dir}/../${mod}"]
}

proc check_module_ok_p {mod from} {
   return [expr {[file exists $from] && [file isdirectory $from]}]
}

proc install_module_1 {mod from install_dir} {
   # Install a generic non-core module.
   # TODO: This probably isn't exactly right for EVERY module...
   # E.g. nsstats/nsstats.tcl should be installed into pages/, not tcl/.

   if {[check_module_ok_p $mod $from]} {
      puts "Installing module '$mod' from '${from}/' into ${install_dir}/"
   } else {
      puts stderr "Error: Bad module copy-from directory:   $from"
      return
   }
   set fl_bin [glob -nocomplain "$from/${mod}*.dll"]
   set fl_lib [glob -nocomplain "$from/${mod}*.lib" "$from/${mod}*.pdb"]
   set fl_tcl [glob -nocomplain "$from/*.tcl" "$from/tcl/*.tcl"]
   if {[llength $fl_bin] > 0} { eval file copy -force -- $fl_bin "$install_dir/bin/" }
   if {[llength $fl_lib] > 0} { eval file copy -force -- $fl_lib "$install_dir/lib/" }
   if {[llength $fl_tcl] > 0} { catch {file mkdir "${install_dir}/tcl/${mod}"} }
   foreach from $fl_tcl { copy_catch $from "${install_dir}/tcl/" }
}


set input_error_p 0 ; set print_help_p 0
if { $argc < 2 } {
   set input_error_p 1 ; set print_help_p 1 ; set arg_list [list]
} else {
   set arg_list [lrange $argv 0 end]
}
if { [catch { set len [llength $arg_list] } errmsg] } {
   set input_error_p 1
   puts stderr "Error:  Optional arguments in arg_list do not form a valid Tcl list."
} elseif { ![even_p $len] } {
   set input_error_p 1
   puts stderr "Error:  arg_list must be a list of key/value pairs, but instead is:\n$arg_list"
} elseif { [catch {array set {AA} $arg_list} errmsg] } {
   set input_error_p 1
   puts stderr "Error:  For arg_list, array set failed with:  $errmsg"
}

set navi_p $AA(-N)
set core_dir [get_from_core_dir $AA(-f)]
set install_dir [get_install_dir $AA(-t) $AA(-c)]
set module_l [list] ; foreach mod $AA(-m) {
   if {[check_module_ok_p $mod [get_module_from_dir $mod $core_dir]]} {
      lappend module_l $mod
   }
}

if {! $AA(-i) || $AA(-h)} { set print_help_p 1 }
if {$print_help_p} {
   puts [get_usage_help $argv0 $core_dir $install_dir $module_l] ; exit -1
} elseif {$input_error_p} { exit -1 }

# TODO: Check that any user-specified directories are actually good?
# TODO: Add a warning if the install directory already exists?

if { ! $navi_p } { puts stderr "Error:  AOLserver is not supported, NaviServer only." ; exit -1 }
puts " Source dir:  $core_dir/"
puts "Install dir:  $install_dir/"
puts "Copying files now..."
install_create_subdirs $install_dir $navi_p
install_naviserver_core $core_dir $install_dir

foreach mod $module_l {
   install_module_1 $mod [get_module_from_dir $mod $core_dir] $install_dir
}
