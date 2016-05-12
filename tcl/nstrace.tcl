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
# This file implements set of commands and utilities to manage
# Tcl interpreter initialization for NaviServer.
#
# What all this stuff does is simple: synthetize a Tcl script
# used to initialize new Tcl interpreters.
#
#
# There are basically two strategies:
#
#  A. Run an introspective script against an initialized
#     startup interpreter and collect definitions of some
#     "known" things: loaded packages, created Tcl procs,
#     namespaces and namespaced variables. Then stuff all
#     this data in a (potentially large) script and run 
#     this script against virgin Tcl interp.
#     This script is obtained by the [nstrace::statescript]
#     command (see below).
#
#
#  B. Register traces on selected Tcl commands and get state
#     they create in a set of shared variables (the epoch). 
#     Then start bootstraping the interp. This will trigger
#     trace callbacks and they will start filling the epoch.
#     After the bootstrapping is done, synthetize a script 
#     containing minimal fixed state (variables, modules) and
#     a definition of [unknown] command which will on-demand
#     load procedure definitions out of the epoch state.
#     This script is obtained by the [nstrace::tracescript]
#     command (see below).
#
#
# Which one of the above 2 strategies is currently used by the 
# server, is controlled by the "lazyloader" parameter of the Tcl 
# library, as defined in the server configuration file.
# The A. strategy is selected by setting the parameter to false.
# The B. strategy is selected by setting the parameter to true.
#
#
# In order to influence script generation, users can add their
# own tracing implementations. Tracers and other supporting
# callbacks for the following Tcl commands are provided per
# default:
# 
#     load, namespace, variable, proc, rename
#
# For the information of how to add new tracers please look
# into the source code of already provided callbacks.
# 
#
# Summary of commands:
#
#   nstrace::enabletrace   activates registered Tcl command traces
#   nstrace::disabletrace  terminates tracing of Tcl commands
#   nstrace::tracescript   returns a script for initializing interps
#
#   nstrace::enablestate   activates generation of the state script
#   nstrace::disablestate  terminates generation of the state script
#   nstrace::statescript   returns a script for initializing interps
#
#   nstrace::isactive      returns true if tracing Tcl commands is on
#   nstrace::config        setup some configuration options
#
#   nstrace::excludensp    skip serializing the given namespace
#   nstrace::namespaces    returns list of namespaces for the given parent
#
#   nstrace::addtrace      registers custom tracer callback
#   nstrace::addscript     registers custom script generator
#   nstrace::addresolver   registers custom command resolver
#
#   nstrace::enablecode    returns signal about start of tracing
#   nstrace::disablecode   returns signal about stop of tracing
#
#   nstrace::addentry      adds one entry into the named trace store
#   nstrace::getentry      returns the entry value from the named store
#   nstrace::delentry      removes the entry from the named store
#   nstrace::getentries    returns all entries from the named store
#
# Limitations:
#
#   o. [namespace forget] is still not implemented
#   o. [namespace origin cmd] breaks if cmd is not already defined
#   o. [info procs] does not return list of all cached procedures
#

ns_runonce {

    namespace eval nstrace {

        variable tvers 0
        variable elock [ns_mutex create traceepochmutex]

        # Private variables
        variable resolvers ""     ; # List of resolvers
        variable tracers   ""     ; # List of tracers
        variable scripts   ""     ; # List of script gegerators
        variable exclnsp   ""     ; # List of namespaces to exclude
        variable enabled    0     ; # True if trace is enabled
        variable config           ; # Array with config options
        variable epoch     -1     ; # The initialization epoch
        
        # Private namespaces
        namespace eval resolve "" ; # Commands for resolving commands
        namespace eval trace   "" ; # Commands registered for tracing
        namespace eval script  "" ; # Commands for generating scripts
        
        # Exported commands
        namespace export unknown

        # Initialize nstrace shared state
        if {[nsv_array exists nstrace] == 0} {
            nsv_set nstrace lastepoch $epoch
            nsv_set nstrace epochlist ""
        }
        
        # Allow creation of interp initialization epochs
        set config(-doepochs)  1

        #
        # Used to set/get nstrace options.
        #

        proc config {args} {
            variable config
            if {[llength $args] == 0} {
                array get config
            } elseif {[llength $args] == 1} {
                set config([lindex $args 0])
            } else {
                set config([lindex $args 0]) [lindex $args 1]
            }
        }

        #
        # Starts the tracing session by passing the enable code
        # to all registered trace callbacks
        #

        proc enabletrace {} {
            variable config
            variable tracers
            variable enabled
            incr enabled 1
            if {$enabled > 1} {
                return
            }
            if {$config(-doepochs) != 0} {
                variable epoch [_newepoch]
            }
            set nsp [namespace current]
            set on  [enablecode]
            foreach trace $tracers {
                ${nsp}::trace::_$trace ${nsp}::trace::_$trace $on
            }
        }

        #
        # Stopts the tracing session by passing the diaable code
        # to all registered trace callbacks
        #

        proc disabletrace {} {
            variable enabled
            variable tracers
            incr enabled -1
            if {$enabled > 0} {
                return
            }
            set nsp [namespace current]
            set off [disablecode]
            foreach trace $tracers {
                ${nsp}::trace::_$trace ${nsp}::trace::_$trace $off
            }
        }

        #
        # Starts the interp state gathering. This step prepares
        # the stage for the [statescript] command.
        #

        proc enablestate {} {
            variable tracers
            set nsp [namespace current]
            set on  [enablecode]
            #
            # Activate [rename] and [load] tracers so we can 
            # catch renaming commands and loading packages.
            #
            foreach trace $tracers {
                if {$trace eq {rename} || $trace eq {load}} {
                    ${nsp}::trace::_$trace ${nsp}::trace::_$trace $on
                }
            }
        }

        #
        # Stops the intep state gathering. After this call, we can
        # safely call [statescript] to get the blueprint script.
        #

        proc disablestate {} {
            variable tracers
            set nsp [namespace current]
            set off [disablecode]
            #
            # Disable activated [rename] / [load] tracers
            #
            foreach trace $tracers {
                if {$trace eq {rename} || $trace eq {load}} {
                    ${nsp}::trace::_$trace ${nsp}::trace::_$trace $off
                }
            }
        }

        #
        # Returns code to signal trace enable. This command is mainly
        # used from trace callbacks as [enabletrace] will call each 
        # callback with this code to signalize start of tracing.
        # 

        proc enablecode {} {
            return 126
        }

        #
        # Returns code to signal trace disable. This command is mainly 
        # used from trace callbacks as [disabletrace] will call each
        # callback with this code to signalize end of tracing.
        #

        proc disablecode {} {
            return 127
        }

        #
        # Returns true if the tracing mechanism is on
        #

        proc isactive {} {
            variable enabled
            expr {$enabled > 0}
        }

        #
        # This one synthetizes script used to pull state out of the 
        # shared variables filled in by the Tcl command tracing.
        #

        proc tracescript {{file ""}} {
            variable epoch
            variable scripts

            set script {}
            set import {}

            #
            # Serialize nstrace namespace
            # as we need it loaded always.
            #

            foreach n [namespaces [namespace current]] {
                foreach {s i} [_serializensp $n] {
                    if {[string length $s]} {
                        append script "namespace eval [list $n] {" \n
                        append script $s  \n
                        append script "}" \n
                    }
                    if {[string length $i]} {
                        append import "namespace eval [list $n] {" \n
                        append import $i  \n
                        append import "}" \n
                    }
                }
            }

            #
            # Invoke script generators
            #

            foreach cmd $scripts {
                append script [script::_$cmd] \n
            }

            #
            # Add imported commands
            #

            if {[string length $import]} {
                append script $import \n
            }
            
            #
            # Tell to use current initialization epoch
            #

            append script "namespace eval [list [namespace current]] {" \n
            append script "_useepoch $epoch" \n
            append script "}" \n

            #
            # Script is output to file mainly for
            # interactive debugging purposes.
            #

            if {$file ne ""} {
                _savescript $file $script
            } else {
                return $script
            }
        }

        #
        # This one generates full-blown script with entire 
        # interpreter state.
        #

        proc statescript {{file ""}} {
            variable scripts

            set script {}
            set import {}

            #
            # Invoke [load] script generator first
            # as this must pull up all loaded mods.
            #

            foreach cmd $scripts {
                if {$cmd eq {load}} {
                    append script [script::_$cmd] \n
                }
            }

            #
            # Invoke rest of script generators, but skip
            # [rename] as this will be loaded last.
            #

            foreach cmd $scripts {
                if {$cmd ne {load} && $cmd ne {rename}} {
                    append script [script::_$cmd] \n
                }
            }

            #
            # Serialize all known namespaces. At this
            # point user callbacks have possibly masked
            # some of the existing namespaces.
            #

            # Filter nsf namespaces from the list of all namespaces,
            # except the one from XOTcl or from the next Scripting
            # Framework. The filter clauses are designed to work with
            # XOTcl 1.* and the Next Scripting Framework (i.e. XOTcl
            # 2.0 and NX).
            #
            set nsps [list]
            if {[info commands ::nsf::object::exists] ne ""} {
                # NX, XOTcl 2
                set xotcl 2
                foreach n [namespaces] {
                    if {$n eq "::nsf" 
                        || [string match "::nsf::*" $n]
                        || [::nsf::object::exists $n]} { continue }
                    lappend nsps $n
                }
            } elseif {[info commands ::xotcl::Object] ne ""} {
                # XOTcl 1
                set xotcl 1
                foreach n [namespaces] {
                    if {[string match "::xotcl*" $n]
                        || [::xotcl::Object isobject $n]} { continue} 
                    lappend nsps $n
                }
            } else {
                set xotcl 0
                set nsps [namespaces]
            }
            
            #puts stderr "remaining namespaces [join [lsort $nsps] \n]"

            # Serialize the remaining namespaces
            foreach n $nsps {
                foreach {s i} [_serializensp $n] {
                    if {[string length $s]} {
                        append script "namespace eval [list $n] {" \n
                        append script $s  \n
                        append script "}" \n
                    }
                    if {[string length $i]} {
                        append import "namespace eval [list $n] {" \n
                        append import $i  \n
                        append import "}" \n
                    }
                }
            }

            #
            # Invoke [rename] script generators before XOTcl to allow
            # it to overload content...
            #
            foreach cmd $scripts {
                if {$cmd eq {rename}} {
                    append script [script::_$cmd] \n
                }
            }

            if {$xotcl > 0} {
                #
                # Serialize XOTcl/NX content
                # 
                if {[catch {::Serializer all} objects]} {
                    ns_log notice "NX/XOTcl extension not loaded; will not copy objects\
                      (error: $objects; $::errorInfo)."
                    set objects ""
                } else {
                    append script \n "namespace import -force ::xotcl::*" \n $objects \n
                }
            }

            #
            # Import commands from other namespaces
            #

            if {[string length $import]} {
                append script $import \n
            }

            #
            # Invoke [rename] script generators last
            # ... deactivated by GN

            #foreach cmd $scripts {
            #    if {$cmd eq {rename}} {
            #      append script [script::_$cmd] \n
            #    }
            #}

            #
            # Script is output to file mainly for
            # interactive debugging purposes. The first line
            # gives you always the latest version, the second
            # one is useful for debugging e.g. ns_eval.
            
            #if {1} {_savescript /tmp/__ns_blueprint.tcl $script}
            #if {1} {_savescript /tmp/__ns_blueprint[clock format [clock seconds] -format %d-%b-%Y-%H:%M:%S].tcl $script}

            if {$file ne ""} {
                _savescript $file $script
            } else {
                return $script
            }
        }

        #
        # This is used to exclude Tcl namespace definition from the
        # inclusion in the blueprint script. Some Tcl extensions
        # (mainly OO-type) handle their own namespaces which can't 
        # be easily handled by the generic serialization script.
        # Such namespaces may contain additional client data which
        # is not visible from the Tcl level thus can't be simply
        # serialized by a Tcl level script.
        #
        # This is NOT affecting [tracescript] collection!
        #

        proc excludensp {nsp} {
            variable exclnsp
            if {[lsearch $exclnsp $nsp] == -1} {
                lappend exclnsp $nsp
            }
        }

        #
        # Registers custom tracer callback.
        #

        proc addtrace {cmd arglist body} {
            variable tracers
            if {[lsearch $tracers $cmd] == -1} {
                lappend tracers $cmd
                set tracer [namespace current]::trace::_$cmd
                proc $tracer $arglist $body
                if {[isactive]} {
                    if {[info commands $cmd] ne {}} {
                        trace add execution $cmd leave $tracer
                    } else {
                        $tracer $tracer [enablecode]
                    }
                }
                return $tracer
            }
        }

        #
        # Registers script-creator callback. Such callbacks
        # are called by the [nstrace::tracescript] or
        # [nstrace::statescript] to create blueprint script
        # used to initialize interp.
        #
        
        proc addscript {cmd body} {
            variable scripts
            if {[lsearch $scripts $cmd] == -1} {
                lappend scripts $cmd
                set cmd [namespace current]::script::_$cmd
                proc $cmd args $body
                return $cmd
            }
        }

        #
        # Registes resolver callback. Such callbacks are
        # called by the [nstrace::unknown] procedure to
        # locate requested item in one of the trace stores.
        #

        proc addresolver {cmd arglist body} {
            variable resolvers
            if {[lsearch $resolvers $cmd] == -1} {
                lappend resolvers $cmd
                set cmd [namespace current]::resolve::$cmd
                proc $cmd $arglist $body
                return $cmd
            }
        }

        #
        # Adds one item definition 
        # to the named trace store
        #

        proc addentry {store var val} {
            variable epoch
            nsv_set nstrace-${store}-${epoch} $var $val
        }

        #
        # Deletes one item definition
        # from the named trace store
        #

        proc delentry {store var} {
            variable epoch
            nsv_unset -nocomplain nstrace-${store}-${epoch} $var
        }

        #
        # Get item definition from 
        # the named trace store 
        #

        proc getentry {store var} {
            variable epoch
            
            if {[info exists ::errorInfo]} {set savedErrorInfo $::errorInfo}
            if {[info exists ::errorCode]} {set savedErrorCode $::errorCode}

            if {[catch {nsv_set nstrace-${store}-${epoch} $var} val]} {
                if {[info exists savedErrorInfo]} {
                    set ::errorInfo $savedErrorInfo
                } else {
                    unset -nocomplain ::errorInfo
                }
                if {[info exists savedErrorCode]} {
                    set ::errorCode $savedErrorCode
                } else {
                    unset -nocomplain ::errorCode
                }
                set val ""
            }
            return $val
        }

        #
        # List items in the named trace store
        #

        proc getentries {store {pattern *}} {
            variable epoch
            nsv_array names nstrace-${store}-${epoch} $pattern
        }

        #
        # This command overlays the standard Tcl [unknown] 
        # command. It is used to locate and re-generate
        # the item definition out of the state captured in
        # thred shared variables. It invokes registered 
        # resolver procedures one by one until the item
        # is located. If unable to locate the item, the
        # control is passed to the underlying Tcl [unknown].
        #

        proc unknown {args} {
            set cmd [lindex $args 0]
            if {[uplevel nstrace::_resolve [list $cmd]]} {
                set c [catch {uplevel 1 $cmd [lrange $args 1 end]} r]
            } else {
                set c [catch {::eval ::tcl::unknown $args} r]
            }
            return -code $c -errorcode $::errorCode -errorinfo $::errorInfo $r
        }

        #
        # Returns the list of namespaces starting with the 
        # given namespace and working down the namespace tree.
        #

        proc namespaces {{top "::"}} {
            variable nsplist
            set nsplist ""
            _namespaces $top
            set result $nsplist
            set nsplist ""
            return $result
        }

        #
        # Helper procedure for [namespaces]
        #

        proc _namespaces {top} {
            variable nsplist
            lappend nsplist $top
            foreach nsp [namespace children $top] {
                #if {$nsp eq "::tcl"} {continue}
                _namespaces $nsp
            }
        }

        #
        # helper proc for ensembles
        # reconfigures rather than recreates existing ensembles
        # to prevent loss of bytecoding
        # 

        proc _create_or_config_ensemble {cmd cfg} {
            if {[info commands $cmd] eq $cmd && [namespace ensemble exists $cmd]} {
                uplevel 1 [list ::namespace ensemble configure $cmd {*}$cfg]
            } else {
                uplevel 1 [list ::namespace ensemble create -command $cmd {*}$cfg]
            }
        }

        #
        # helper proc for ensemble serialization
        #
        # Tcl versions before 8.5 do not have a namespace ensemble
        # command.  NaviServer does not support on the Tcl layer older
        # versions than 8.5, but if someone wants to check e.g. some
        # basic properties with a Tcl 8.4 version, it should not break
        # due to this small change. So we guard the definition of the
        # ensemble serialization by checking Tcl's version number.
        
        if {$::tcl_version >= 8.5} {
            proc _getensemble {cmd} {
                if {[namespace ensemble exists $cmd]} {
                    set _cfg [namespace ensemble configure $cmd]
                    set _enns [dict get $_cfg -namespace]
                    dict unset _cfg -namespace
                    set _encmd [list ::nstrace::_create_or_config_ensemble $cmd $_cfg]
                    return [list namespace eval $_enns $_encmd]\n
                }
            }
        } else {
            proc _getensemble {cmd} {
                # Do nothing.
            }
        }

        #
        # Generates scipts to re-generate namespace definition.
        # Returns two scripts: first is used to re-generate 
        # namespace with all its procedures and variables
        # and second is used to import commands/procedures
        # from other namespaces.
        #
        # Normally the import script must be included after
        # all namespace scripts for all namespaces have been
        # collected as they will actually generate places
        # where commands are/will-be imported from.
        #

        proc _serializensp {nsp} {
            variable exclnsp
            foreach nn $exclnsp {
                if {[string match $nn $nsp]} {
                    return
                }
            }
            set script {}
            set import {}

            # If $nsp is empty (no vars, no procs), we create at 
            # least a 
            #    namespace eval $nsp {}
            # entry by adding the space.
            append script " "
            
            #
            # Keep the variables of all namespaces except these of "::"
            #
            if {$nsp ne "::"} {
                foreach vn [info vars ${nsp}::*] {
                    append script [_varscript $vn]
                }
            }

            #
            # Save procs and command of all namespaces
            #
            foreach pn [info procs ${nsp}::*] {
                set orig [namespace origin $pn]
                if {$orig ne [namespace which -command $pn]} {
                    append import "namespace import -force [list $orig]" \n
                } else {
                    append script [_procscript $pn]
                }
            }

            # Add aliases
            foreach cmd [interp aliases {}] {
                if {[namespace qualifiers $cmd] eq $nsp} {
                    append script "interp alias {} $cmd {} [interp alias {} $cmd]" \n
                }
            }    

            foreach cn [info commands ${nsp}::*] {
                set orig [namespace origin $cn]
                if {[info procs $cn] eq {} &&
                    $orig ne [namespace which -command $cn]} {
                    append import "namespace import -force [list $orig]" \n
                }
                append import [_getensemble $cn]
            }
            foreach ex [namespace eval $nsp [list namespace export]] {
                append script "namespace export [list $ex]" \n
            }
            return [list $script $import]
        }

        #
        # Helper to return a script to re-generate Tcl procedure.
        # Caller must wrap this script into [namespace eval] 
        # command as the procedure will not generate the procedure
        # under fully qualified name.
        #

        proc _procscript {cmd} {
            set pargs {}
            foreach arg [info args $cmd] {
                if {![info default $cmd $arg def]} {
                    lappend pargs $arg
                } else {
                    lappend pargs [list $arg $def]
                }
            }
            set pname [namespace tail $cmd]
            set pbody [info body $cmd]
            append script "proc [list $pname] [list $pargs] [list $pbody]" \n
        }

        #
        # Helper to return a script to re-generate Tcl variable.
        # Caller must wrap this script into [namespace eval]
        # command as the procedure will not generate the variable
        # under fully qualified name.
        #

        proc _varscript {var} {
            set vname [namespace tail $var]
            if {[array exists $var]} {
                append script "variable [list $vname]" \n
                append script "array set [list $vname] [list [array get $var]]" \n
            } elseif {[info exists $var]} {
                append script "variable [list $vname] [list [set $var]]" \n
            } else {
                # maybe a variable without a value; no need to preserve it
            }
        }

        #
        # Helper procedure to save a script to a file.
        # This is mainly used for debugging.
        #

        proc _savescript {file script} {
            if {[catch {open $file w} chan] == 0} {
                puts $chan $script
                close $chan
            }
        }

        #
        # Procedure invoking registered resolvers to lookup
        # the given command. First resolver which successfully
        # resolves the command stops the resolving process.
        #

        proc _resolve {cmd} {
            variable resolvers
            foreach resolver $resolvers {
                if {[uplevel 1 [info comm resolve::$resolver] [list $cmd]]} {
                    return 1
                }
            }
            return 0
        }

        #
        # List ID's of all known threads in the process.
        # This is used for epoch management.
        #

        proc _getthreads {} {
            set threads ""
            foreach entry [ns_info threads] {
                lappend threads [lindex $entry 2]
            }
            return $threads
        }

        #
        # Creates new tracing epoch by copying the current
        # tracing state into new set of shared variables.
        # The old epoch remains active until there are any
        # threads active which still use the old state.
        #

        proc _newepoch {} {
            variable elock
            ns_mutex lock $elock
            set old [nsv_set  nstrace lastepoch]
            set new [nsv_incr nstrace lastepoch]
            nsv_lappend nstrace $new [ns_thread id]
            if {$old >= 0} {
                _copyepoch $old $new
                _delepochs
            }
            nsv_lappend nstrace epochlist $new
            ns_mutex unlock $elock
            return $new
        }

        #
        # Helper procedure to _newepoch to copy the trace
        # state from one to another set of shared variables.
        #

        proc _copyepoch {old new} {
            foreach var [nsv_names nstrace-*-${old}] {
                set cmd [lindex [split $var -] 1]
                nsv_array reset nstrace-${cmd}-${new} [nsv_array get $var]
            }
        }

        #
        # Delete tracing epochs when they are not referenced
        # by any active thread.
        #

        proc _delepochs {} {
            set tlist [_getthreads]
            set elist ""
            foreach epoch [nsv_set nstrace epochlist] {
                if {[_delepoch $epoch $tlist] == 0} {
                    lappend elist $epoch
                } else {
                    nsv_unset nstrace $epoch
                }
            }
            nsv_set nstrace epochlist $elist
        }

        #
        # Helper procedure to _delepochs. It conditionally
        # deletes one trace epoch which is not referenced
        # by any active thread.
        #

        proc _delepoch {epoch threads} {
            set self [ns_thread id] 
            foreach tid [nsv_set nstrace $epoch] {
                if {$tid ne $self && [lsearch $threads $tid] >= 0} {
                    lappend alive $tid
                }
            }
            if {[info exists alive]} {
                nsv_set nstrace $epoch $alive
                return 0
            } else {
                foreach var [nsv_names nstrace-*-${epoch}] {
                    nsv_unset $var
                }
                return 1
            }
        }

        #
        # Procedure used to select one specific epoch. This is 
        # normally part of the blueprint script generated by 
        # the [nstrace::tracescript] procedure.
        #

        proc _useepoch {epoch} {
            if {$epoch >= 0} {
                set tid [ns_thread id]
                if {[lsearch [nsv_set nstrace $epoch] $tid] == -1} {
                    nsv_lappend nstrace $epoch $tid
                }
            }
        }        
    }

    #
    # The code below provides implementation of tracing callbacks
    # for following Tcl commands:
    # 
    #    [namespace]
    #    [variable]
    #    [load]
    #    [proc]
    #    [rename]
    #
    # Those callbacks are needed to support basic introspection
    # capabilities for Tcl commands/packages. For customization,
    # users can supply their own tracers on-the-fly.
    #

    #
    # Register the [load] trace. This will create 
    # the following key/value pair in the "load" store:
    #
    #  --- key ----              --- value ---
    #  <path_of_loaded_image>    <name_of_the_init_proc>
    #
    # We normally need only the name_of_the_init_proc for
    # being able to load the package in other interpreters,
    # but we store the path to the image file as well.
    #

    nstrace::addtrace load {cmdline code args} {
        if {$code != 0} {
            if {$code == [nstrace::enablecode]} {
                trace add execution load leave $cmdline
            } elseif {$code == [nstrace::disablecode]} {
                trace remove execution load leave $cmdline
            }
            return
        }
        set image [lindex $cmdline 1]
        set iproc [lindex $cmdline 2]
        if {$iproc eq {}} {
            foreach pkg [info loaded] {
                if {[lindex $pkg 0] eq $image} {
                    set iproc [lindex $pkg 1]
                }
            }
        }
        nstrace::addentry load $image $iproc
    }

    nstrace::addscript load {
        append script \n
        # Load all traced packages
        foreach image [nstrace::getentries load] {
            set iproc [nstrace::getentry load $image]
            append script "load {} [list $iproc]" \n
            set loaded($image) 1
        }
        # Load all the rest missed by tracing
        foreach pkg [info loaded] {
            set image [lindex $pkg 0]
            if {![info exists loaded($image)]} {
                set iproc [lindex $pkg 1]
                append script "load {} [list $iproc]" \n
            }
        }
        return $script
    }

    #
    # Register the [namespace] trace. This will create 
    # the following key/value entry in "namespace" store:
    #
    #  --- key ----                   --- value ---
    #  ::fully::qualified::namespace  1
    #
    # It will also fill the "proc" store for procedures
    # and commands imported in this namespace with following:
    #
    #  --- key ----                   --- value ---
    #  ::fully::qualified::proc       [list <ns>  "" ""]
    #
    # The <ns> is the name of the namespace where the 
    # command or procedure is imported from.
    #

    nstrace::addtrace namespace {cmdline code args} {
        if {$code != 0} {
            if {$code == [nstrace::enablecode]} {
                trace add execution namespace leave $cmdline
            } elseif {$code == [nstrace::disablecode]} {
                trace remove execution namespace leave $cmdline
            }
            return
        }
        set nop [lindex $cmdline 1]
        set cns [uplevel namespace current]
        if {$cns eq {::}} {
            set cns {}
        }
        switch -glob -- $nop {
            eva* {
                set nsp [lindex $cmdline 2]
                if {![string match {::*} $nsp]} {
                    set nsp ${cns}::$nsp
                }
                nstrace::addentry namespace $nsp 1
            }
            imp* {
                # - parse import arguments (skip opt "-force")
                set opts [lrange $cmdline 2 end]
                if {[string match {-fo*} [lindex $opts 0]]} {
                    set opts [lrange $cmdline 3 end]
                }
                # - register all imported procs and commands
                foreach opt $opts {
                    if {![string match {::*} [::namespace qual $opt]]} {
                        set opt ${cns}::$opt
                    }
                    # - first import procs
                    foreach entry [nstrace::getentries proc $opt] {
                        set cmd ${cns}::[::namespace tail $entry]
                        set nsp [::namespace qual $entry]
                        set done($cmd) 1
                        set entry [list 0 $nsp {} {}]
                        nstrace::addentry proc $cmd $entry
                    }

                    # - then import commands
                    foreach entry [info commands $opt] {
                        set cmd ${cns}::[::namespace tail $entry]
                        set nsp [::namespace qual $entry]
                        if {[info exists done($cmd)] == 0} {
                            set entry [list 0 $nsp {} {}]
                            nstrace::addentry proc $cmd $entry
                        }
                    }
                }
            }
        }
    }

    nstrace::addscript namespace {
        append script \n
        foreach entry [nstrace::getentries namespace] {
            append script "namespace eval [list $entry] {}" \n
        }
        return $script
    }

    #
    # Register the [variable] trace. This will create 
    # the following key/value entry in the "variable" store:
    #
    #  --- key ----                   --- value ---
    #  ::fully::qualified::variable   1
    #
    # The variable value itself is ignored at the time
    # of trace/collection. Instead, we take the real
    # value at the time of script generation.
    #

    nstrace::addtrace variable {cmdline code args} {
        if {$code != 0} {
            if {$code == [nstrace::enablecode]} {
                trace add execution variable leave $cmdline
            } elseif {$code == [nstrace::disablecode]} {
                trace remove execution variable leave $cmdline
            }
            return
        }
        set opts [lrange $cmdline 1 end]
        if {[llength $opts]} {
            set cns [uplevel namespace current]
            if {$cns eq {::}} {
                set cns {}
            }
            foreach {var val} $opts {
                if {![string match {::*} $var]} {
                    set var ${cns}::$var
                }
                nstrace::addentry variable $var 1
            }
        }
    }

    nstrace::addscript variable {
        append script \n
        foreach entry [nstrace::getentries variable] {
            set nsp [namespace qual $entry]
            set var [namespace tail $entry]
            append script "namespace eval [list $nsp] {" \n
            append script "variable [list $var]"
            if {[array exists $entry]} {
                append script \n "array set [list $var] [list [array get $entry]]" \n
            } elseif {[info exists $entry]} {
                append script " [list [set $entry]]" \n 
            } else {
                append script \n
            }
            append script "}" \n
        }
        return $script
    }


    #
    # Register the [rename] trace. It will create 
    # the following key/value pair in "rename" store:
    #
    #  --- key ----              --- value ---
    #  ::fully::qualified::old  ::fully::qualified::new
    #
    # The "new" value may be empty, for commands that 
    # have been deleted. In such cases we also remove
    # any traced procedure definitions.
    #

    nstrace::addtrace rename {cmdline code args} {
        if {$code != 0} {
            if {$code == [nstrace::enablecode]} {
                trace add execution rename leave $cmdline
            } elseif {$code == [nstrace::disablecode]} {
                trace remove execution rename leave $cmdline
            }
            return
        }
        set cns [uplevel namespace current]
        if {$cns eq {::}} {
            set cns {}
        }
        set old [lindex $cmdline 1]
        if {![string match {::*} $old]} {
            set old ${cns}::$old
        }
        set new [lindex $cmdline 2]
        if {$new ne {}} {
            if {![string match {::*} $new]} {
                set new ${cns}::$new
            }
            nstrace::addentry rename $old $new
        } else {
            nstrace::delentry proc $old
        }
    }

    nstrace::addscript rename {
        append script \n
        foreach old [nstrace::getentries rename] {
            set new [nstrace::getentry rename $old]
            #
            # $old and $new might be procs or commands.
            # Handle only handle those cases, where neither
            # $old or $new is a proc, since the procs are already
            # parts of the serialized blueprint.
            #
            if {"[info procs $old][info procs $new]" eq ""} {
                append script "rename [list $old] [list $new]" \n
            }
        }
        return $script
    }

    #
    # Register the [proc] trace. This will create 
    # the following key/value pair in the "proc" store:
    #
    #  --- key ----              --- value ---
    #  ::fully::qualified::proc  [list <epoch> <ns> <arglist> <body>]
    #
    # The <epoch> chages anytime one (re)defines a proc. 
    # The <ns> is the namespace where the command was imported 
    # from. If empty, the <arglist> and <body> will hold the 
    # actual procedure definition. See the "namespace" tracer
    # implementation also.
    #

    nstrace::addtrace proc {cmdline code args} {
                                                if {$code != 0} {
                                                    if {$code == [nstrace::enablecode]} {
                                                        trace add execution proc leave $cmdline
                                                    } elseif {$code == [nstrace::disablecode]} {
                                                        trace remove execution proc leave $cmdline
                                                    }
                                                    return
                                                }
                                                set cns [uplevel namespace current]
                                                if {$cns eq {::}} {
                                                    set cns {}
                                                }
                                                set cmd [lindex $cmdline 1]
                                                if {![string match {::*} $cmd]} {
                                                    set cmd ${cns}::$cmd
                                                }
                                                set pbody [info body $cmd]
                                                set pargs {}
                                                foreach arg [info args $cmd] {
                                                    if {![info default $cmd $arg def]} {
                                                        lappend pargs $arg
                                                    } else {
                                                        lappend pargs [list $arg $def]
                                                    }
                                                }
                                                set pdef [nstrace::getentry proc $cmd]
                                                if {$pdef eq {}} {
                                                    set epoch -1 ; # never traced before
                                                } else {
                                                    set epoch [lindex $pdef 0]
                                                }
                                                nstrace::addentry proc $cmd [list [incr epoch] {} $pargs $pbody]
                                            }

    nstrace::addscript proc {
        if {[llength [nstrace::getentries proc]] == 0} {
            return
        }
        return {
            if {[info commands ::tcl::unknown] eq {}} {
                rename ::unknown ::tcl::unknown
                namespace import -force ::nstrace::unknown
            }
            if {[info commands ::tcl::info] eq {}} {
                rename ::info ::tcl::info
            }
            proc ::info args {
                set cmd [lindex $args 0]
                set hit [lsearch -glob {commands procs args default body} $cmd*]
                if {$hit > 1} {
                    if {[catch {uplevel 1 ::tcl::info $args}]} {
                        uplevel 1 nstrace::_resolve [list [lindex $args 1]]
                    }
                    return [uplevel 1 ::tcl::info $args]
                }
                if {$hit == -1} {
                    return [uplevel 1 ::tcl::info $args]
                }
                set cns [uplevel 1 namespace current]
                if {$cns eq {::}} {
                    set cns {}
                }
                set pat [lindex $args 1]
                if {![string match {::*} $pat]} {
                    set pat ${cns}::$pat
                }
                set fns [nstrace::getentries proc $pat]
                if {[string match $cmd* commands]} {
                    set fns [concat $fns [nstrace::getentries xotcl $pat]]
                }
                foreach entry $fns {
                    if {$cns ne [namespace qual $entry]} {
                        set lazy($entry) 1
                    } else {
                        set lazy([namespace tail $entry]) 1
                    }
                }
                foreach entry [uplevel ::tcl::info $args] {
                    set lazy($entry) 1
                }
                array names lazy
            }
        }
    }

    #
    # The proc resolver will try to resolve the command
    # in the current namespace first, and if not found,
    # in global namespace. It also handles commands 
    # imported from other namespaces.
    #

    nstrace::addresolver resolveprocs {cmd {export 0}} {
        set cns [uplevel namespace current]
        set name [namespace tail $cmd]
        if {$cns eq {::}} {
            set cns {}
        }
        if {![string match {::*} $cmd]} {
            set ncmd ${cns}::$cmd
            set gcmd ::$cmd
        } else {
            set ncmd $cmd
            set gcmd $cmd
        }
        set pdef [nstrace::getentry proc $ncmd]
        if {$pdef eq {}} {
            set pdef [nstrace::getentry proc $gcmd]
            if {$pdef eq {}} {
                return 0
            }
            set cmd $gcmd
        } else {
            set cmd $ncmd
        }
        set epoch [lindex $pdef 0]
        set pnsp  [lindex $pdef 1]
        if {$pnsp ne {}} {
            set nsp [namespace qual $cmd]
            if {$nsp eq {}} {
                set nsp ::
            }
            set cmd ${pnsp}::$name
            if {[resolveprocs $cmd 1] == 0} {
                return 0
            }
            namespace eval $nsp [list namespace import -force $cmd]
        } else {
            uplevel 0 [list ::proc $cmd [lindex $pdef 2] [lindex $pdef 3]]
            if {$export} {
                set nsp [namespace qual $cmd]
                if {$nsp eq {}} {
                    set nsp ::
                }
                namespace eval $nsp [list namespace export $name]
            }
        }
        variable resolveproc
        set resolveproc($cmd) $epoch
        return 1
    }
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
