# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Startup-only infrastructure. Provenance is shared per virtual server, not
# serialized into every interpreter. Execution traces carry only a local token.

namespace eval ::ns_package_provenance {}

proc ns_package_provenance {command args} {
    if {$command ne "get" || [llength $args] ni {1 2}} {
        error {wrong # args: should be "ns_package_provenance get package ?version?"}
    }
    set name [lindex $args 0]
    set version [expr {[llength $args] == 2 ? [lindex $args 1] : [package provide $name]}]
    set key [list package $name $version]
    if {[nsv_exists ns_package_provenance $key]} {
        return [nsv_get ns_package_provenance $key]
    }
    return {}
}

proc ::ns_package_provenance::origin {} {
    set origin [dict create kind source file {} line 0 proc {} packages {}]
    # Do not retain command text or arguments: they can contain credentials.
    for {set i 1} {$i < [info frame]} {incr i} {
        set frame [info frame -$i]
        if {[dict exists $frame proc]
            && [string match ::ns_package_provenance::* [dict get $frame proc]]} {continue}
        if {[dict get $origin proc] eq {} && [dict exists $frame proc]} {
            dict set origin proc [dict get $frame proc]
        }
        if {[dict exists $frame file]} {
            dict set origin file [dict get $frame file]
            dict set origin line [dict get $frame line]
            break
        }
    }
    if {[dict get $origin file] eq {}} {dict set origin kind dynamic}
    return $origin
}

proc ::ns_package_provenance::merge {name version field value {first {}}} {
    set key [list package $name $version]
    # All read/modify/write steps use the same NSV-array lock.
    nsv_array eval ns_package_provenance {
        set record [expr {[nsv_exists ns_package_provenance $key]
                          ? [nsv_get ns_package_provenance $key]
                          : [dict create providedBy {} requiredBy {} firstLoadedBy {}]}]
        if {$field ne {} && $value ni [dict get $record $field]} {
            dict lappend record $field $value
        }
        if {$first ne {} && [dict get $record firstLoadedBy] eq {}} {
            dict set record firstLoadedBy $first
        }
        nsv_set ns_package_provenance $key $record
    }
}

proc ::ns_package_provenance::provided {} {
    set result {}
    foreach name [package names] {
        set version [package provide $name]
        if {$version ne {}} {dict set result $name $version}
    }
    return $result
}

proc ::ns_package_provenance::provider {name version origin stackKey directCaller} {
    set stack [expr {[nsv_exists ns_package_provenance $stackKey]
                     ? [nsv_get ns_package_provenance $stackKey] : {}}]
    for {set i [expr {[llength $stack]-1}]} {$i >= 0} {incr i -1} {
        set frame [lindex $stack $i]
        if {[dict get $frame type] eq "require"} {
            dict set origin packages [list [dict get $frame name]]
            dict lappend frame provided [list $name $version]
            nsv_set ns_package_provenance $stackKey [lreplace $stack $i $i $frame]
            merge $name $version providedBy $origin
            return
        }
    }
    merge $name $version providedBy $origin $directCaller
}

proc ::ns_package_provenance::event {token subject command args} {
    set operation [lindex $args end]
    if {$subject eq "ns_ictl"} {
        if {[lindex $command 1] eq "update"} {
            foreach target {package load} {
                set callback [list ::ns_package_provenance::observe $token $target]
                if {$operation eq "enter"} {
                    trace remove execution ::$target {enter leave} $callback
                } else {
                    trace add execution ::$target {enter leave} $callback
                }
            }
        }
        return
    }
    set packageCommand [expr {$subject eq "package"}]
    set subcommand [lindex $command 1]
    if {$packageCommand} {
        if {$subcommand ne {} && [string first $subcommand require] == 0} {
            set type require
            set index [expr {[lindex $command 2] eq "-exact" ? 3 : 2}]
            set name [lindex $command $index]
        } elseif {$subcommand ne {} && [string first $subcommand provide] == 0
                  && [llength $command] == 4} {
            set type provide
            set name [lindex $command 2]
        } else {return}
    } else {set type load}
    set stackKey [list stack $token]
    set stack [expr {[nsv_exists ns_package_provenance $stackKey]
                     ? [nsv_get ns_package_provenance $stackKey] : {}}]
    if {$operation eq "enter"} {
        if {$type eq "provide"} {return}
        set caller [origin]
        if {[llength $stack]} {
            # A load frame is infrastructure, not a package dependency.
            foreach parent [lreverse $stack] {
                if {[dict get $parent type] eq "require"} {
                    dict set caller packages [list [dict get $parent name]]
                    break
                }
            }
        }
        if {$type eq "require"} {
            set frame [dict create type require name $name before [package provide $name] caller $caller provided {}]
            dict set frame requested [lrange $command [expr {$index+1}] end]
            dict set frame exact [expr {$index == 3}]
        } else {
            set index 1
            while {[lindex $command $index] in {-global -lazy}} {incr index}
            if {[lindex $command $index] eq "--"} {incr index}
            set provider [dict create kind native file [lindex $command $index] \
                              init [lindex $command [expr {$index+1}]]]
            set frame [dict create type load before [provided] caller $caller provider $provider]
        }
        nsv_set ns_package_provenance $stackKey [lappend stack $frame]
        return
    }
    set code [lindex $args 0]
    if {$type eq "provide"} {
        if {$code != 0} {return}
        set location [origin]
        provider $name [lindex $command 3] $location $stackKey $location
        return
    }
    if {![llength $stack]} {return}
    set frame [lindex $stack end]
    set stack [lrange $stack 0 end-1]
    if {[llength $stack]} {
        nsv_set ns_package_provenance $stackKey $stack
    } else {nsv_unset ns_package_provenance $stackKey}
    if {$code != 0} {return}
    if {$type eq "require"} {
        set version [package provide $name]
        if {$version eq {}} {return}
        set caller [dict get $frame caller]
        set required $caller
        dict set required requested [dict get $frame requested]
        dict set required exact [dict get $frame exact]
        set first [expr {[dict get $frame before] eq {} ? $caller : {}}]
        merge $name $version requiredBy $required $first
        # A loader can also provide aliases which were never separately required.
        foreach provision [dict get $frame provided] {
            lassign $provision providedName providedVersion
            merge $providedName $providedVersion {} {} $caller
        }
    } else {
        set before [dict get $frame before]
        set location [dict get $frame provider]
        dict for {name version} [provided] {
            if {![dict exists $before $name] || [dict get $before $name] ne $version} {
                provider $name $version $location $stackKey [dict get $frame caller]
            }
        }
    }
}

proc ::ns_package_provenance::observe {token subject command args} {
    # Observation must not alter package results or the caller's error state.
    set hadInfo [info exists ::errorInfo]
    if {$hadInfo} {set savedInfo $::errorInfo}
    set hadCode [info exists ::errorCode]
    if {$hadCode} {set savedCode $::errorCode}
    catch {event $token $subject $command {*}$args}
    if {$hadInfo} {set ::errorInfo $savedInfo} else {unset -nocomplain ::errorInfo}
    if {$hadCode} {set ::errorCode $savedCode} else {unset -nocomplain ::errorCode}
    return
}

proc ::ns_package_provenance::install {} {
    # Repeated allocation/reinitialization must not install duplicate traces.
    foreach item [trace info execution ::package] {
        if {[lindex [lindex $item 1] 0] eq "::ns_package_provenance::observe"} {return}
    }
    if {![nsv_array exists ns_package_provenance]} {nsv_array set ns_package_provenance {}}
    dict for {name version} [provided] {
        set key [list package $name $version]
        if {![nsv_exists ns_package_provenance $key]} {
            set unknown [dict create kind preexisting]
            merge $name $version providedBy $unknown $unknown
        }
    }
    set token [list [ns_thread id] [clock clicks]]
    foreach command {::package ::load ::ns_ictl} {
        trace add execution $command {enter leave} [list ::ns_package_provenance::observe $token [namespace tail $command]]
    }
}

proc ::ns_package_provenance::uninstall {} {
    foreach command {::package ::load ::ns_ictl} {
        foreach item [trace info execution $command] {
            lassign $item operations callback
            if {[lindex $callback 0] eq "::ns_package_provenance::observe"} {
                trace remove execution $command $operations $callback
                set key [list stack [lindex $callback 1]]
                if {[nsv_exists ns_package_provenance $key]} {nsv_unset ns_package_provenance $key}
            }
        }
    }
}
