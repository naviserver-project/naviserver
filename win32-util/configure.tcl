#!/bin/sh
# The next line restarts using tclsh. \
exec tclsh "$0" "$@"

# by Andrew Piskorski <atp@piskorski.com>
#
# To compile on Windows we need "include/nsversion.h", which is
# auto-generated from "include/nsversion.h.in", but we do not have a
# working autoconf/configure on Windows!  If you've run configure on
# Linux, it does work fine to simply use the "include/nsversion.h"
# it generates on Windows as well.  But if you can't (or don't want to)
# do that, run this Tcl script on Windows instead.
#


proc check_paths_ok_p_nsversion {} {
   set in_l [list configure.ac include/nsversion.h.in]
   set ok_p 1
   foreach ff $in_l { if {![file exists $ff] || [file isdirectory $ff]} {
      set ok_p 0
      puts stderr "Error: Input file missing:  $ff"
   }}
   if {! $ok_p} { puts stderr {Warning: Are you in the wrong directory?} }
   return $ok_p
}

proc get_vars_nsversion {} {
   set f_in {configure.ac}
   set ac_vars [list @NS_MAJOR_VERSION@ @NS_MINOR_VERSION@ @NS_RELEASE_SERIAL@ @NS_RELEASE_LEVEL@ @NS_VERSION@ @NS_PATCH_LEVEL@ @NAVISERVER@]
   # @NAVISERVER@ is supposed to be the install directory, but on Windows
   # we do not determine that until install time.
   set DD [dict create]
   set hh [open $f_in r] ; set b1 [read -nonewline $hh] ; close $hh
   return [parse_ac_text $ac_vars $b1]
}

proc write_file_nsversion {DD} {
   set f_in  {include/nsversion.h.in}
   set f_out {include/nsversion-win32.h}
   set hh [open $f_in r] ; set content [read -nonewline $hh] ; close $hh
   set output [replace_ac_text $DD $content]
   set hh [open $f_out w] ; fconfigure $hh -translation lf ; puts -nonewline $hh $output ; close $hh
   puts "Wrote file:  $f_out"
}


proc parse_ac_text {ac_vars b1 {debug_p 0}} {
   return [parse_ac_subst [parse_ac_grep $ac_vars $b1 $debug_p] $debug_p]
}

proc parse_ac_grep {ac_vars b1 {debug_p 0}} {
   set b2 [join [regexp -all -inline -- {[\n\r]AC_SUBST[^\n\r]*} $b1] "\n"]
   foreach v1 $ac_vars {
      set v2 [string trim $v1 {@}]
      set patt {} ; append patt {AC_SUBST\(\[} $v2 {\],[ ]*([^\n\r\#\)]*)}
      if {$debug_p} { puts "     v2:  $v2   Pattern:  $patt" }
      if {[regexp $patt $b2 match m1]} {
         set rr [string trim $m1 {][}]
         if {$debug_p} { puts " Result:  $rr" }
         dict set DD $v2 $rr
      } else {
         dict set DD $v2 {unknown}
      }
   }
   return $DD
}

proc parse_ac_subst {DD {debug_p 0}} {
   set update_l [list]
   foreach [list key val] $DD {
      if {![regexp -- {[$]} $val]} { continue }
      if {$debug_p} { puts "${key}  --->  ${val}" }
      dict with DD {
         if {[catch { set r1 [subst $val] } errmsg]} {
            # Probably we do not have the variable it is referring to.
            # Make a final attempt (hack) in this single purpose
            # script for NS_PATCH_LEVEL.
            if {$key eq "NS_PATCH_LEVEL"} {
                set r1 [subst {"$NS_MAJOR_VERSION.$NS_MINOR_VERSION.$NS_RELEASE_SERIAL"}]
            } else {
                if {$debug_p} { puts "  --->  $errmsg" }
                set r1 {unknown}
            }
         }
         if {$debug_p} { puts "  --->  $r1" }
         lappend update_l $key $r1
      }
   }
   foreach [list key val] $update_l { dict set DD $key $val }
   return $DD
}


proc replace_ac_text {DD content {debug_p 0}} {
   set output {}
   set v_patt {@([^@]+)@} ; set q_patt {"@[^@]+@}  ;#"

   foreach line [split $content "\n"] {
      if {![regexp -- {@} $line]} {
         append output $line "\n" ; continue
      }
      set v1 {} ; if {$debug_p} { puts $line }
      if {[regexp -- $v_patt $line foo v1] && [dict exists $DD $v1]} {
         set v2 [dict get $DD $v1]
         if {{unknown} eq $v2} {
            if {[regexp $q_patt $line]} {
               # Looks like the variable is ALREADY surrounded by double quotes in
               # the source file, leave it as-is.
            } else {
               # Must surround the string value in quotes to make it legal C code:
               # TODO: If our unknown value is supposed to be numeric the string
               # emitted here will probably break the C compiler.  Check how?

               set v2 "\"${v2}\""
            }
         }
         if {$debug_p} { puts "  $v1  --->  $v2" }
         append output [regsub -- $v_patt $line $v2] "\n"
      } else {
         append output $line "\n"
      }
   }
   return $output
}


if {[check_paths_ok_p_nsversion]} { write_file_nsversion [get_vars_nsversion] }
