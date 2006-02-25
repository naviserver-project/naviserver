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
# $Header$
#

#
# User level commands:
#
#   ttrace::eval           top-level wrapper (ttrace-savvy eval)
#   ttrace::enable         activates registered Tcl command traces
#   ttrace::disable        terminates tracing of Tcl commands
#   ttrace::isenabled      returns true if ttrace is enabled
#   ttrace::cleanup        bring the interp to a pristine state
#   ttrace::update         update interp to the latest trace epoch
#   ttrace::config         setup some configuration options
#   ttrace::getscript      returns a script for initializing interps
#
# Commands used for/from trace callbacks:
#
#   ttrace::atenable       register callback to be done at trace enable
#   ttrace::atdisable      register callback to be done at trace disable
#   ttrace::addtrace       register user-defined tracer callback
#   ttrace::addscript      register user-defined script generator
#   ttrace::addresolver    register user-defined command resolver
#   ttrace::addcleanup     register user-defined cleanup procedures
#   ttrace::addentry       adds one entry into the named trace store
#   ttrace::getentry       returns the entry value from the named store
#   ttrace::delentry       removes the entry from the named store
#   ttrace::getentries     returns all entries from the named store
#   ttrace::preload        register procedures to be preloaded always
#
#
# Limitations:
#
#   o. [namespace forget] is still not implemented
#   o. [namespace origin cmd] breaks if cmd is not already defined
#   o. [info procs] does not return list of all cached procedures
#

ns_runonce {

    namespace eval ttrace {

        # Setup some compatibility wrappers
        variable tvers 0
        variable elock [ns_mutex create traceepochmutex]
        
        # Package variables
        variable resolvers ""     ; # List of registered resolvers
        variable tracers   ""     ; # List of registered cmd tracers
        variable scripts   ""     ; # List of registered script makers
        variable enables   ""     ; # List of trace-enable callbacks
        variable disables  ""     ; # List of trace-disable callbacks
        variable preloads  ""     ; # List of procedure names to preload
        variable enabled   0      ; # True if trace is enabled
        variable config           ; # Array with config options
        
        variable epoch     -1     ; # The initialization epoch
        variable cleancnt   0     ; # Counter of registered cleaners
        
        # Package private namespaces
        namespace eval resolve "" ; # Commands for resolving commands
        namespace eval trace   "" ; # Commands registered for tracing
        namespace eval enable  "" ; # Commands invoked at trace enable
        namespace eval disable "" ; # Commands invoked at trace disable
        namespace eval script  "" ; # Commands for generating scripts
        
        # Exported commands
        namespace export unknown
        
        # Initialize ttrace shared state
        if {[nsv_array exists ttrace] == 0} {
            nsv_set ttrace lastepoch $epoch
            nsv_set ttrace epochlist ""
        }
        
        # Allow creation of interp initialization epochs
        set config(-doepochs) 1

        proc eval {cmd args} {
            set nmsp [::uplevel namespace current]
            enable
            set code [catch {
                namespace eval $nmsp ::uplevel [list $cmd $args]
            } result]
            disable
            if {$code == 0} {
                ns_ictl save [getscript]
            } else {
                ns_ictl markfordelete
            }
            return -code $code $result
        }

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

        proc enable {} {
            variable config
            variable tracers
            variable enables
            variable enabled
            incr enabled 1
            if {$enabled > 1} {
                return
            }
            if {$config(-doepochs) != 0} {
                variable epoch [_newepoch]
            }
            set nsp [namespace current]
            foreach enabler $enables {
                enable::_$enabler
            }
            foreach trace $tracers {
                if {[info commands $trace] ne ""} {
                    trace add execution $trace leave ${nsp}::trace::_$trace
                }
            }
        }

        proc disable {} {
            variable enabled
            variable tracers
            variable disables
            incr enabled -1
            if {$enabled > 0} {
                return
            }
            set nsp [namespace current]
            foreach disabler $disables {
                disable::_$disabler
            }
            foreach trace $tracers {
                if {[info commands $trace] ne ""} {
                    trace remove execution $trace leave ${nsp}::trace::_$trace
                }
            }
        }

        proc isenabled {} {
            variable enabled
            expr {$enabled > 0}
        }

        proc update {{from -1}} {
            if {$from == -1} { 
                variable epoch [nsv_set ttrace lastepoch]
            } else {
                if {[lsearch [nsv_set ttrace epochlist] $from] == -1} {
                    error "no such epoch: $from"
                }
                variable epoch $from
            }
            uplevel [getscript]
        } 

        proc getscript {} {
            variable preloads
            variable epoch
            variable scripts
            append script [_serializensp] \n
            append script "::namespace eval [namespace current] {" \n
            append script "::namespace export unknown" \n
            append script "_useepoch $epoch" \n
            append script "}" \n
            foreach cmd $preloads {
                append script [_serializeproc $cmd] \n
            }
            foreach maker $scripts {
                append script [script::_$maker]
            }
            return $script
        }

        proc cleanup {args} {
            foreach cmd [info commands resolve::cleaner_*] {
                uplevel $cmd $args
            }
        }

        proc preload {cmd} {
            variable preloads
            if {[lsearch $preloads $cmd] == -1} {
                lappend preloads $cmd
            }
        }

        proc atenable {cmd arglist body} {
            variable enables
            if {[lsearch $enables $cmd] == -1} {
                lappend enables $cmd
                set cmd [namespace current]::enable::_$cmd
                proc $cmd $arglist $body
                return $cmd
            }
        }

        proc atdisable {cmd arglist body} {
            variable disables
            if {[lsearch $disables $cmd] == -1} {
                lappend disables $cmd
                set cmd [namespace current]::disable::_$cmd
                proc $cmd $arglist $body
                return $cmd
            }
        }

        proc addtrace {cmd arglist body} {
            variable tracers
            if {[lsearch $tracers $cmd] == -1} {
                lappend tracers $cmd
                set tracer [namespace current]::trace::_$cmd
                proc $tracer $arglist $body
                if {[isenabled]} {
                    trace add execution $cmd leave $tracer
                }
                return $tracer
            }
        }

        proc addscript {cmd body} {
            variable scripts
            if {[lsearch $scripts $cmd] == -1} {
                lappend scripts $cmd
                set cmd [namespace current]::script::_$cmd
                proc $cmd args $body
                return $cmd
            }
        }

        proc addresolver {cmd arglist body} {
            variable resolvers
            if {[lsearch $resolvers $cmd] == -1} {
                lappend resolvers $cmd
                set cmd [namespace current]::resolve::$cmd
                proc $cmd $arglist $body
                return $cmd
            }
        }

        proc addcleanup {body} {
            variable cleancnt
            set cmd [namespace current]::resolve::cleaner_[incr cleancnt]
            proc $cmd args $body
            return $cmd
        }

        proc addentry {cmd var val} {
            variable epoch
            nsv_set ${epoch}-$cmd $var $val
        }

        proc delentry {cmd var} {
            variable epoch
            set ei $::errorInfo
            set ec $::errorCode
            catch {nsv_unset ${epoch}-$cmd $var}
            set ::errorInfo $ei
            set ::errorCode $ec
        }

        proc getentry {cmd var} {
            variable epoch
            set ei $::errorInfo
            set ec $::errorCode
            if {[catch {nsv_set ${epoch}-$cmd $var} val]} {
                set ::errorInfo $ei
                set ::errorCode $ec
                set val ""
            }
            return $val
        }

        proc getentries {cmd {pattern *}} {
            variable epoch
            nsv_array names ${epoch}-$cmd $pattern
        }

        proc unknown {args} {
            set cmd [lindex $args 0]
            if {[uplevel ttrace::_resolve [list $cmd]]} {
                set c [catch {uplevel $cmd [lrange $args 1 end]} r]
            } else {
                set c [catch {::eval ::tcl::unknown $args} r]
            }
            return -code $c -errorcode $::errorCode -errorinfo $::errorInfo $r
        }

        proc _resolve {cmd} {
            variable resolvers
            foreach resolver $resolvers {
                if {[uplevel [info comm resolve::$resolver] [list $cmd]]} {
                    return 1
                }
            }
            return 0
        }

        proc _getthreads {} {
            set threads ""
            foreach entry [ns_info threads] {
                lappend threads [lindex $entry 2]
            }
            return $threads
        }

        proc _newepoch {} {
            variable elock
            ns_mutex lock $elock
            set old [nsv_set  ttrace lastepoch]
            set new [nsv_incr ttrace lastepoch]
            nsv_lappend ttrace $new [ns_thread getid]
            if {$old >= 0} {
                _copyepoch $old $new
                _delepochs
            }
            nsv_lappend ttrace epochlist $new
            ns_mutex unlock $elock
            return $new
        }

        proc _copyepoch {old new} {
            foreach var [nsv_names $old-*] {
                set cmd [lindex [split $var -] 1]
                nsv_array reset $new-$cmd [nsv_array get $var]
            }
        }

        proc _delepochs {} {
            set tlist [_getthreads]
            set elist ""
            foreach epoch [nsv_set ttrace epochlist] {
                if {[_delepoch $epoch $tlist] == 0} {
                    lappend elist $epoch
                } else {
                    nsv_unset ttrace $epoch
                }
            }
            nsv_set ttrace epochlist $elist
        }

        proc _delepoch {epoch threads} {
            set self [ns_thread getid] 
            foreach tid [nsv_set ttrace $epoch] {
                if {$tid != $self && [lsearch $threads $tid] >= 0} {
                    lappend alive $tid
                }
            }
            if {[info exists alive]} {
                nsv_set ttrace $epoch $alive
                return 0
            } else {
                foreach var [nsv_names $epoch-*] {
                    nsv_unset $var
                }
                return 1
            }
        }

        proc _useepoch {epoch} {
            if {$epoch >= 0} {
                set tid [ns_thread getid]
                if {[lsearch [nsv_set ttrace $epoch] $tid] == -1} {
                    nsv_lappend ttrace $epoch $tid
                }
            }
        }

        proc _serializeproc {cmd} {
            set dargs [info args $cmd]
            set pbody [info body $cmd]
            set pargs ""
            foreach arg $dargs {
                if {![info default $cmd $arg def]} {
                    lappend pargs $arg
                } else {
                    lappend pargs [list $arg $def]
                }
            }
            set nsp [namespace qualifier $cmd]
            if {$nsp eq ""} {
                set nsp "::"
            }
            append res "::namespace eval $nsp {" \n
            append res "::proc [namespace tail $cmd] [list $pargs] [list $pbody]" \n
            append res "}" \n
        }

        proc _serializensp {{nsp ""} {result _}} {
            upvar $result res
            if {$nsp eq ""} {
                set nsp [namespace current]
            }
            append res "::namespace eval $nsp {" \n
            foreach var [info vars ${nsp}::*] {
                set vname [namespace tail $var]
                if {[array exists $var] == 0} {
                    append res "::variable $vname {[set $var]}" \n
                } else {
                    append res "::variable $vname" \n
                    append res "::array set $vname [list [array get $var]]" \n
                }
            }
            foreach cmd [info procs ${nsp}::*] {
                append res [_serializeproc $cmd] \n
            }
            append res "}" \n
            foreach nn [namespace children $nsp] {
                _serializensp $nn res
            }
            return $res
        }
    }
    
    #
    # The code below is ment to be run once during the application start. 
    # It provides implementation of tracing callbacks for some Tcl commands.
    # Users can supply their own tracer implementations on-the-fly.
    #
    # The code below will create traces for the following Tcl commands:
    #    "namespace", "variable", "load", "proc" and "rename"
    #
    # Also, the Tcl object extension XOTcl 1.3.8 is handled and all XOTcl
    # related things, like classes and objects are traced (many thanks to
    # Gustaf Neumann from XOTcl for his kind help and support). 
    #
    
    #
    # Register the "load" trace. This will create 
    # the following key/value pair in the "load" store:
    #
    #  --- key ----              --- value ---
    #  <path_of_loaded_image>    <name_of_the_init_proc>
    #
    # We normally need only the name_of_the_init_proc for
    # being able to load the package in other interpreters,
    # but we store the path to the image file as well.
    #

    ttrace::addtrace load {cmdline code args} {
        if {$code != 0} {
            return
        }
        set image [lindex $cmdline 1]
        set iproc [lindex $cmdline 2]
        if {$iproc eq ""} {
            foreach pkg [info loaded] {
                if {[lindex $pkg 0] == $image} {
                    set iproc [lindex $pkg 1]
                }
            }
        }
        ttrace::addentry load $image $iproc
    }

    ttrace::addscript load {
        append res "\n"
        # Load all traced packages
        foreach image [ttrace::getentries load] {
            set iproc [ttrace::getentry load $image]
            append res "::load {} $iproc" \n
            set loaded($image) 1
        }
        # Load all the rest missed by trace
        foreach pkg [info loaded] {
            set image [lindex $pkg 0]
            if {![info exists loaded($image)]} {
                set iproc [lindex $pkg 1]
                append res "::load {} $iproc" \n
            }
        }
        return $res
    }

    #
    # Register the "namespace" trace. This will create 
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

    ttrace::addtrace namespace {cmdline code args} {
        if {$code != 0} {
            return
        }
        set nop [lindex $cmdline 1]
        set cns [uplevel namespace current]
        if {$cns eq "::"} {
            set cns ""
        }
        switch -glob $nop {
            eva* {
                set nsp [lindex $cmdline 2]
                if {![string match "::*" $nsp]} {
                    set nsp ${cns}::$nsp
                }
                ttrace::addentry namespace $nsp 1
            }
            imp* {
                # - parse import arguments (skip opt "-force")
                set opts [lrange $cmdline 2 end]
                if {[string match "-fo*" [lindex $opts 0]]} {
                    set opts [lrange $cmdline 3 end]
                }
                # - register all imported procs and commands
                foreach opt $opts {
                    if {![string match "::*" [::namespace qual $opt]]} {
                        set opt ${cns}::$opt
                    }
                    # - first import procs
                    foreach entry [ttrace::getentries proc $opt] {
                        set cmd ${cns}::[::namespace tail $entry]
                        set nsp [::namespace qual $entry]
                        set done($cmd) 1
                        set entry [list 0 $nsp "" ""]
                        ttrace::addentry proc $cmd $entry
                    }

                    # - then import commands
                    foreach entry [info commands $opt] {
                        set cmd ${cns}::[::namespace tail $entry]
                        set nsp [::namespace qual $entry]
                        if {[info exists done($cmd)] == 0} {
                            set entry [list 0 $nsp "" ""]
                            ttrace::addentry proc $cmd $entry
                        }
                    }
                }
            }
        }
    }

    ttrace::addscript namespace {
        append res \n
        foreach entry [ttrace::getentries namespace] {
            append res "::namespace eval $entry {}" \n
        }
        return $res
    }

    #
    # Register the "variable" trace. This will create 
    # the following key/value entry in the "variable" store:
    #
    #  --- key ----                   --- value ---
    #  ::fully::qualified::variable   1
    #
    # The variable value itself is ignored at the time
    # of trace/collection. Instead, we take the real
    # value at the time of script generation.
    #

    ttrace::addtrace variable {cmdline code args} {
        if {$code != 0} {
            return
        }
        set opts [lrange $cmdline 1 end]
        if {[llength $opts]} {
            set cns [uplevel namespace current]
            if {$cns eq "::"} {
                set cns ""
            }
            foreach {var val} $opts {
                if {![string match "::*" $var]} {
                    set var ${cns}::$var
                }
                ttrace::addentry variable $var 1
            }
        }
    }

    ttrace::addscript variable {
        append res \n
        foreach entry [ttrace::getentries variable] {
            set cns [namespace qual $entry]
            set var [namespace tail $entry]
            append res "::namespace eval $cns {" \n
            append res "::variable $var"
            if {[array exists $entry]} {
                append res "\n::array set $var [list [array get $entry]]" \n
            } elseif {[info exists $entry]} {
                append res " [list [set $entry]]" \n 
            } else {
                append res \n
            }
            append res } \n
        }
        return $res
    }


    #
    # Register the "rename" trace. It will create 
    # the following key/value pair in "rename" store:
    #
    #  --- key ----              --- value ---
    #  ::fully::qualified::old  ::fully::qualified::new
    #
    # The "new" value may be empty, for commands that 
    # have been deleted. In such cases we also remove
    # any traced procedure definitions.
    #

    ttrace::addtrace rename {cmdline code args} {
        if {$code != 0} {
            return
        }
        set cns [uplevel namespace current]
        if {$cns eq "::"} {
            set cns ""
        }
        set old [lindex $cmdline 1]
        if {![string match "::*" $old]} {
            set old ${cns}::$old
        }
        set new [lindex $cmdline 2]
        if {$new ne ""} {
            if {![string match "::*" $new]} {
                set new ${cns}::$new
            }
            ttrace::addentry rename $old $new
        } else {
            ttrace::delentry proc $old
        }
    }

    ttrace::addscript rename {
        append res \n
        foreach old [ttrace::getentries rename] {
            set new [ttrace::getentry rename $old]
            append res "::rename $old {$new}" \n
        }
        return $res
    }

    #
    # Register the "proc" trace. This will create 
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

    ttrace::addtrace proc {cmdline code args} {
        if {$code != 0} {
            return
        }
        set cns [uplevel namespace current]
        if {$cns eq "::"} {
            set cns ""
        }
        set cmd [lindex $cmdline 1]
        if {![string match "::*" $cmd]} {
            set cmd ${cns}::$cmd
        }
        set dargs [info args $cmd]
        set pbody [info body $cmd]
        set pargs ""
        foreach arg $dargs {
            if {![info default $cmd $arg def]} {
                lappend pargs $arg
            } else {
                lappend pargs [list $arg $def]
            }
        }
        set pdef [ttrace::getentry proc $cmd]
        if {$pdef eq ""} {
            set epoch -1 ; # never traced before
        } else {
            set epoch [lindex $pdef 0]
        }
        ttrace::addentry proc $cmd [list [incr epoch] "" $pargs $pbody]
    }

    ttrace::addscript proc {
        return {
            if {[info command ::tcl::unknown] eq ""} {
                rename ::unknown ::tcl::unknown
                namespace import -force ::ttrace::unknown
            }
            if {[info command ::tcl::info] eq ""} {
                rename ::info ::tcl::info
            }
            proc ::info args {
                set cmd [lindex $args 0]
                set hit [lsearch -glob {commands procs args default body} $cmd*]
                if {$hit > 1} {
                    if {[catch {uplevel ::tcl::info $args}]} {
                        uplevel ttrace::_resolve [list [lindex $args 1]]
                    }
                    return [uplevel ::tcl::info $args]
                }
                if {$hit == -1} {
                    return [uplevel ::tcl::info $args]
                }
                set cns [uplevel namespace current]
                if {$cns eq "::"} {
                    set cns ""
                }
                set pat [lindex $args 1]
                if {![string match "::*" $pat]} {
                    set pat ${cns}::$pat
                }
                set fns [ttrace::getentries proc $pat]
                if {[string match $cmd* commands]} {
                    set fns [concat $fns [ttrace::getentries xotcl $pat]]
                }
                foreach entry $fns {
                    if {$cns != [namespace qual $entry]} {
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
    # Register procedure resolver. This will try to
    # resolve the command in the current namespace
    # first, and if not found, in global namespace.
    # It also handles commands imported from other
    # namespaces.
    #

    ttrace::addresolver resolveprocs {cmd {export 0}} {
        set cns [uplevel namespace current]
        set name [namespace tail $cmd]
        if {$cns eq "::"} {
            set cns ""
        }
        if {![string match "::*" $cmd]} {
            set ncmd ${cns}::$cmd
            set gcmd ::$cmd
        } else {
            set ncmd $cmd
            set gcmd $cmd
        }
        set pdef [ttrace::getentry proc $ncmd]
        if {$pdef eq ""} {
            set pdef [ttrace::getentry proc $gcmd]
            if {$pdef eq ""} {
                return 0
            }
            set cmd $gcmd
        } else {
            set cmd $ncmd
        }
        set epoch [lindex $pdef 0]
        set pnsp  [lindex $pdef 1]
        if {$pnsp ne ""} {
            set nsp [namespace qual $cmd]
            if {$nsp eq ""} {
                set nsp ::
            }
            set cmd ${pnsp}::$name
            if {[resolveprocs $cmd 1] == 0 && [info commands $cmd] eq ""} {
                return 0
            }
            namespace eval $nsp "namespace import -force $cmd"
        } else {
            uplevel 0 [list ::proc $cmd [lindex $pdef 2] [lindex $pdef 3]]
            if {$export} {
                set nsp [namespace qual $cmd]
                if {$nsp eq ""} {
                    set nsp ::
                }
                namespace eval $nsp "namespace export $name"
            }
        }
        variable resolveproc
        set resolveproc($cmd) $epoch
        return 1
    }

    #
    # For XOTcl, the entire item introspection/tracing
    # is delegated to XOTcl itself.
    # The xotcl store is filled with this:
    #
    #  --- key ----               --- value ---
    #  ::fully::qualified::item   <body>
    #
    # The <body> is the script used to generate the entire
    # item (class, object). Note that we do not fill in this
    # during code tracing. It is done during the script
    # generation. In this step, only the placeholder is set.
    #
    # NOTE: we assume all XOTcl commands are imported in global namespace
    #

    ttrace::atenable XOTclEnabler {args} {
        if {[info commands ::xotcl::Class] eq ""} {
            return
        }
        if {[info commands _creator] eq ""} {
            ::xotcl::Class create ::xotcl::_creator -instproc create {args} {
                set result [next]
                if {![string match "::xotcl::_*" $result]} {
                    ttrace::addentry xotcl $result ""
                }
                return $result
            }
        }
        ::xotcl::Class instmixin ::xotcl::_creator
    }

    ttrace::atdisable XOTclDisabler {args} {
        if {   [info commands ::xotcl::Class] eq "" 
            || [info commands ::xotcl::_creator] eq ""} {
            return
        }
        ::xotcl::Class instmixin ""
        ::xotcl::_creator destroy
    }

    set resolver [ttrace::addresolver resolveclasses {classname} {
        set cns [uplevel namespace current]
        set script [ttrace::getentry xotcl $classname]
        if {$script eq ""} {
            set name [namespace tail $classname]
            if {$cns eq "::"} {
                set script [ttrace::getentry xotcl ::$name]
            } else {
                set script [ttrace::getentry xotcl ${cns}::$name]
                if {$script eq ""} {
                    set script [ttrace::getentry xotcl ::$name]
                }
            }
            if {$script eq ""} {
                return 0 ; # Peng! No cigar...
            }
        }
        uplevel [list namespace eval $cns $script]
        return 1
    }]

    ttrace::addscript xotcl [subst -nocommands {
        if {![catch {Serializer new} ss]} {
            foreach entry [ttrace::getentries xotcl] {
                if {[ttrace::getentry xotcl \$entry] eq ""} {
                    ttrace::addentry xotcl \$entry [\$ss serialize \$entry]
                }
            }
            \$ss destroy
            return {::xotcl::Class proc __unknown name {$resolver \$name}}
        }
    }]

    #
    # Register callback to be called on cleanup.
    # This will trash lazily loaded procs which
    # have changed since.
    # 

    ttrace::addcleanup {
        variable resolveproc
        foreach cmd [array names resolveproc] {
            set def [ttrace::getentry proc $cmd]
            if {$def ne ""} {
                set new [lindex $def 0]
                set old $resolveproc($cmd)
                if {[info command $cmd] ne "" && $new != $old} {
                    catch {rename $cmd ""}
                }
            }
        }
    }
}

# EOF
