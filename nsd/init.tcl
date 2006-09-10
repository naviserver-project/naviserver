#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://www.mozilla.org/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is AOLserver Code and related documentation
# distributed by AOL.
# 
# The Initial Developer of the Original Code is America Online,
# Inc. Portions created by AOL are Copyright (C) 1999 America Online,
# Inc. All Rights Reserved.
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
#

#
# $Header$
#

#
# init.tcl --
#
#   Core script to initialize a virtual server at startup.
#

ns_log notice "nsd/init.tcl: booting virtual server: [ns_info server]..."

package require Tcl 8.4

#
# __ns_sourcefile --
#
#   Helper procedure to source a script file.
#

proc __ns_sourcefile {file} {

    set msg "nsd/init.tcl: loading $file"
    ns_log debug $msg

    set code [catch {source $file} err]
    if {$code == 1} {
        ns_log error "$msg failed: $err\n$::errorCode\n$::errorInfo"
    }

    return $code
}


#
# __ns_sourcelibs --
#
#   Helper procedure to source the files in module
#   shared and private Tcl libraries.
#   If empty module, server libs are sourced.
#

proc __ns_sourcelibs {{modname ""}} {

    set sharedlib  [eval ns_library shared  [list $modname]]
    set privatelib [eval ns_library private [list $modname]]

    set files ""

    #
    # Append shared files not in private
    # sourcing init.tcl immediately if it exists.
    #

    foreach file [lsort [glob -nocomplain -dir -- $sharedlib *.tcl]] {
        set tail [file tail $file]
        if {$tail eq {init.tcl}} {
            __ns_sourcefile $file
        } elseif {![file exists [file join $privatelib $tail]]} {
            lappend files $file
        }
    }

    #
    # Append private (per-virtual-server) files
    # sourcing init.tcl immediately if it exists.
    #

    foreach file [lsort [glob -nocomplain -dir -- $privatelib *.tcl]] {
        set tail [file tail $file]
        if {$tail eq {init.tcl}} {
            __ns_sourcefile $file
        } else {
            lappend files $file
        }
    }

    foreach file $files {
        __ns_sourcefile $file
    }
}


#
# __ns_sourcemodule --
#
#   Helper procedure to source module libraries.
#

proc __ns_sourcemodule {modname} {

    ns_module name    $modname
    ns_module shared  [ns_library private $modname]
    ns_module private [ns_library shared  $modname]

    __ns_sourcelibs $modname

    ns_module clear
}


#
# See how to replicate state of the startup interpreter 
# to newly created interpreters in connection and other
# threads. 
#
# At the moment there are two choices:
#
#  a. Run an introspective script against initialized
#     interp and collect "things" (variables, procs,
#     packages etc) present. Then synthetize new Tcl
#     script with commands to re-create those "things"
#     in any new interpreter.
#
#     This results in a potentially very large/complex
#     script which takes long time to run, effectively 
#     slowing down thread creation. Also, it consumes 
#     much more memory as all "things" are always loaded
#     in the Tcl interp, needed or not.
#
#     This is the default mode.
#
#
#  b. Register introspection traces on selected set of
#     Tcl commands and capture the state in thread-shared
#     variables. Then synthetize new Tcl script with 
#     overloaded Tcl [unknown] command to load referenced
#     items (procs, packages etc) on as-needed basis out
#     of the captured state.
#
#     This results in a very small script which is quick
#     to load and consume far less memory as "things" are
#     loaded on as-needed basis by the Tcl [unknown] command.
#     However this mode may pose compatibility problems by
#     some init scripts doing "weird" things during the 
#     interp initialization.
# 
#     This mode is defined by setting the config option
#     ns/server/[ns_info server]/tcl/lazyloader to true.
#

source [file join [ns_library shared] nstrace.tcl]

set section ns/server/[ns_info server]/tcl
set use_trace_inits [ns_config -bool $section lazyloader false]

if {$use_trace_inits} {

    #
    # The b. approach
    #

    nstrace::enabletrace

} else {

    #
    # The a. approach
    #

    nstrace::enablestate
}


#
# Initialize the interp.
#

set _allocate_callback {
    ns_ictl update
}

ns_ictl trace allocate $_allocate_callback

#
# Cleanup the interp, performing garbage 
# collection tasks.
#

set _deallocate_callback {

    #
    # Close opened channels
    #

    ns_chan cleanup
    
    foreach f [file channels] {
        if {![string match std* $f]} {
            close $f
        }
    }

    #
    # Destroy global variables. Namespaced variables
    # are left and could introduce unwanted state.
    #

    foreach g [info globals] {
        switch -glob -- $g {
            auto_* -
            tcl_*  -
            argv0  -
            argc   -
            argv   -
            env {
                # Leave some core Tcl vars.
            }
            default {
                uplevel \#0 unset -nocomplain [list $g]
            }
        }
    }

    set ::errorInfo ""
    set ::errorCode ""

    ns_set  cleanup;  # Destroy non-shared sets
    ns_http cleanup;  # Abort any http requests
    ns_ictl cleanup;  # Internal cleanup (e.g,. Ns_TclRegisterDefer's)
}

ns_ictl trace deallocate $_deallocate_callback

#
# ns_reinit --
#
#   Cleanup and initialize an interp. This is used for 
#   long running detached threads to avoid resource 
#   leaks and/or missed state changes, e.g.:
#
#   ns_thread begin {
#       while {1} {
#           ns_reinit
#           # ... long running work ...
#       }
#   }
#

proc ns_reinit {} [subst {
    $_deallocate_callback
    $_allocate_callback
}]


#
# ns_module --
#
#   Set or return information about the currently initializing 
#   module (useful only from within startup files).
#

proc ns_module {key {val ""}} {

    global _module

    switch -- $key {
        name    -
        private -
        library -
        shared  {
            if {$key eq {library}} {
                set key private
            }
            if {$val ne ""} {
                set _module($key) $val
            }
            if {[info exists _module($key)]} {
                set val $_module($key)
            }
        }
        clear {
            unset -nocomplain _module
        }
        default {
            error "ns_module: invalid command: $key"
        }
    }

    return $val
}


#
# Load global binary modules for any virtual server.
#
# Note that global modules are loaded AFTER per-server modules.
# The reason is the nssock module which needs to be initialized
# first per-server and then loaded globally. We'll have to fix
# this nonsense some time later...
#

ns_runonce -global {
    ns_atprestartup {
        set modules [ns_configsection ns/modules]
        if {$modules ne {}} {
             foreach {module file} [ns_set array $modules] {
                 ns_moduleload -global $module $file
             }
        }
    }
}

#
# Load per-virtual-server binary modules
#

ns_runonce {
    set modules [ns_configsection ns/server/[ns_info server]/modules]
    if {$modules ne {}} {
        foreach {module file} [ns_set array $modules] {
            if {[string tolower $file] ne {tcl}} {
                ns_moduleload $module $file
            }
            ns_ictl addmodule $module
        }
    }
}

if {$use_trace_inits} {

    #
    # ns_eval --
    #
    # Used to eval Tcl code which is then 
    # known in all threads.
    #

    proc ns_eval {cmd args} {

        if {$cmd eq {-synch} || $cmd eq {-pending}} {
            # Skip for the compatibility
            set cmd  [lindex $args 0]
            set args [lrange $args 1 end]
        }

        nstrace::enabletrace
        set code [catch {eval [list $cmd] $args} result]
        nstrace::disabletrace

        if {$code == 1} {
            ns_markfordelete
        } else {
            ns_ictl save [nstrace::tracescript]
        }

        return -code $code $result
    }

    #
    # Source the server Tcl libraries.
    #

    __ns_sourcelibs

    #
    # Source the module-specific Tcl libraries.
    #

    foreach module [ns_ictl getmodules] {
        __ns_sourcemodule $module
    }

    #
    # Disable tracing and generate compact
    # script for later interp inits.
    #

    nstrace::disabletrace
    ns_ictl save [nstrace::tracescript]

} else {

    #
    # Create a job queue for ns_eval processing
    # This queue must be a single-server queue;
    # we don't want to be processing multiple 
    # ns_eval requests simultanously.
    #

    ns_runonce {
        ns_job create "ns_eval_q:[ns_info server]" 1
    }

    #
    # ns_eval --
    #
    #   Evaluate a script which should contain
    #   new procs commands and then save the
    #   state of the procs for other interps
    #   to sync with.
    #   If this is called from within interp init
    #   processing, it will devolve to an eval.
    #
    #   If this ever gets moved to a namespace,
    #   the eval will need to be modified to
    #   ensure that the procs aren't defined 
    #   in that namespace.
    #

    proc ns_eval {args} {

        set len [llength $args]
        set sync 0
        if {$len == 0} {
            return
        }
        if {$len > 1 && [lindex $args 0] eq "-synch"} {
            set sync 1
            set args [lreplace $args 0 0]
            incr len -1
        } elseif {[lindex $args 0] eq "-pending"} {
            if {$len != 1} {
                error "ns_eval: command arguments not allowed with -pending"
            }
            set jlist [ns_job joblist [nsv_get _ns_eval_jobq [ns_info server]]]
            set res [list]
            foreach job $jlist {
                array set jstate $job
                set scr $jstate(script)
                # Strip off the constant, non-user supplied cruft
                set scr [lindex $scr 1]
                set stime $jstate(starttime)
                lappend res [list $stime $scr]
            }
            return $res
        }
        if {$len == 1} {
            set args [lindex $args 0]
        }

        #
        # Need to always incorporate given script into current interp
        # Use this also to verify the script prior to doing the fold into
        # the ictl environment
        #

        set code [catch {uplevel 1 _ns_helper_eval $args} result]
        if {!$code && [ns_ictl epoch]} {

            #
            # If the local eval result was ok (code == 0),
            # and if we are not in interp init processing (epoch != 0),
            # eval the args in a fresh thread to obtain a pristine
            # environment.
            # Note that running the _ns_eval must be serialized for this
            # server.  We are handling this by establishing that the
            # ns_job queue handling these requests will run only a single
            # thread.
            #

            set qid [nsv_get _ns_eval_jobq [ns_info server]]
            set scr [list _ns_eval $args]
            if {$sync} {
                set th_code [catch {
                    set job_id [ns_job queue $qid $scr]
                    ns_job wait $qid $job_id
                } th_result]
            } else {
                set th_code [catch {
                    ns_job queue -detached $qid $scr
                } th_result]
            }
            if {$th_code} {
                return -code $th_code $th_result
            }
            
        } elseif {$code == 1} {
            ns_ictl markfordelete
        }

        return -code $code $result
    }


    #
    # _ns_eval --
    #
    #   Internal helper func for ns_eval.  This
    #   function will evaluate the given args (from
    #   a pristine thread/interp that ns_eval put
    #   it into) and then load the result into
    #   the interp init script.
    #

    proc _ns_eval {args} {

        set len [llength $args]
        if {$len == 0} {
            return
        } elseif {$len == 1} {
            set args [lindex $args 0]
        }        
        set code [catch {uplevel 1 _ns_helper_eval $args} result]
        if {$code == 1} {
            # TCL_ERROR: Dump this interp to avoid proc pollution.
            ns_markfordelete
        } else {
            # Save this interp's namespaces for others.
            ns_ictl save [nstrace::statescript]
        }

        return -code $code $result
    }


    #
    # _ns_helper_eval --
    #
    #   This Internal helper func is used by both ns_eval and _ns_eval.
    #   It will insure that any references to ns_eval from code
    #   eval'ed is properly turned into simple evals.
    #

    proc _ns_helper_eval {args} {

        set didsaveproc 0

        if {[info proc _saved_ns_eval] eq ""} {
            rename ns_eval _saved_ns_eval
            proc ns_eval {args} {
                set len [llength $args]
                if {$len == 0} {
                    return
                } elseif {$len == 1} {
                    set args [lindex $args 0]
                }
                uplevel 1 $args
            }
            set didsaveproc 1
        }
        set code [catch {uplevel 1 [eval concat $args]} result]
        if {$didsaveproc} {
            rename ns_eval ""
            rename _saved_ns_eval ns_eval
        }

        return -code $code $result
    }

    #
    # Source the server Tcl libraries
    #

    __ns_sourcelibs

    #
    # Source the module-specific Tcl libraries.
    #

    foreach module [ns_ictl getmodules] {
        __ns_sourcemodule $module
    }

    #
    # Do local cleanup before starting
    # interpreter introspection.
    #

    foreach cmd [info commands __ns_*] {
        rename $cmd ""
    }
    ns_cleanup

    nstrace::disablestate
    ns_ictl save [nstrace::statescript]
}

#
# Kill this interp to save memory.
#

ns_ictl markfordelete

# EOF $RCSfile$
