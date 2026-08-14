#!/usr/bin/env tclsh

#
# gen-config-params.tcl --
#
# Generate doctools include files from NaviServer configuration parameter
# documentation dictionaries.
#

package require nx

namespace eval ::ns_configdoc {}

nx::Class create ::ns_configdoc::ConfigParamGenerator {

    :method init {} {
        set :configFile     [file join /usr/local/ns modules tcl configdoc.tcl]
        set :moduleName     ""
        set :sectionName    ""
        set :title          ""
        set :outputFile     ""
        set :parametersOnly 0
        set :spec           ""
        set :configParamDoc ""
    }

    :public method run {argv} {
        :parseOptions $argv
        :validateOptions
        :loadConfig
        :selectSpec
        :writeFile [:render]
    }

    :method usage {} {
        puts stderr {usage: gen-config-params.tcl --config FILE (--section SECTION | --module MODULE) ?--title TITLE? ?--parameters-only? --output FILE}
        exit 2
    }

    :method optionArgument {argvVar indexVar} {
        upvar 1 $argvVar argv $indexVar i
        incr i
        if {$i >= [llength $argv]} {
            :usage
        }
        return [lindex $argv $i]
    }

    :method parseOptions {argv} {
        for {set i 0} {$i < [llength $argv]} {incr i} {
            set opt [lindex $argv $i]
            switch -- $opt {
                --config {
                    set :configFile [:optionArgument argv i]
                }
                --section {
                    set :sectionName [:optionArgument argv i]
                }
                --module {
                    set :moduleName [:optionArgument argv i]
                }
                --title {
                    set :title [:optionArgument argv i]
                }
                --parameters-only -
                --params-only {
                    set :parametersOnly 1
                }
                --output {
                    set :outputFile [:optionArgument argv i]
                }
                default {
                    :usage
                }
            }
        }
    }

    :method validateOptions {} {
        set modes 0
        foreach value [list ${:sectionName} ${:moduleName}] {
            if {$value ne ""} {
                incr modes
            }
        }

        if {${:configFile} eq "" || ${:outputFile} eq "" || $modes != 1} {
            :usage
        }
    }

    :method loadConfig {} {
        source ${:configFile}

        if {![info exists ::ns_configdoc::data]} {
            error "configuration documentation dictionary ::ns_configdoc::data not defined by ${:configFile}"
        }

        set :configParamDoc $::ns_configdoc::data
    }

    :method selectSpec {} {
        if {${:sectionName} ne ""} {
            if {![dict exists ${:configParamDoc} sections ${:sectionName}]} {
                error "section '${:sectionName}' not found in ::ns_configdoc::data"
            }
            set :spec [dict get ${:configParamDoc} sections ${:sectionName}]
            return
        }

        if {![dict exists ${:configParamDoc} modules ${:moduleName}]} {
            error "module '${:moduleName}' not found in ::ns_configdoc::data"
        }
        set :spec [dict get ${:configParamDoc} modules ${:moduleName}]
    }

    :method render {} {
        set spec ${:spec}
        set fallbackTitle [:fallbackTitle]
        set effectiveTitle [expr {${:title} ne "" ? ${:title} : [:metaGet $spec title $fallbackTitle]}]

        set out ""

        if {!${:parametersOnly}} {
            append out "\[comment {This file is generated. Do not edit manually.}\]\n\n"
            append out "\[subsection {[:fmtInline $effectiveTitle]}]\n\n"
            append out [:renderIntro $spec]
        }

        append out [:renderParameterList $spec]
        return $out
    }

    :method fallbackTitle {} {
        expr {${:sectionName} ne "" ? ${:sectionName} : ${:moduleName}}
    }

    :method fallbackSee {} {
        expr {${:moduleName} ne "" ? [list [list module ${:moduleName}]] : ""}
    }

    :method renderIntro {spec} {
        set out ""

        set desc [:metaGet $spec desc ""]
        if {$desc ne ""} {
            append out [:fmtDocBlock $desc] "\n"
        }

        set included [:metaGet $spec include ""]
        if {$included ne ""} {
            append out "\[para]\n"
            append out "This module also accepts the parameters documented for "
            append out "[:fmtConst $included]. The parameters listed below are specific to "
            append out "[:fmtConst ${:moduleName}].\n\n"
        }

        append out [:renderScope [:metaGet $spec scope ""]]

        set example [:metaGet $spec example ""]
        if {$example ne ""} {
            append out "\[example_begin]\n"
            append out [:dedent $example]
            append out "\n\[example_end]\n\n"
        }

        set see [:metaGet $spec see [:fallbackSee]]
        if {$see ne ""} {
            append out [:renderSee $spec $see]
        }

        return $out
    }

    :method renderScope {scope} {
        set out ""

        switch -- $scope {
            {global server} {
                append out "\[para]\n"
                append out "This module can be loaded globally through \[const ns/modules\]"
                append out " or per-server through \[const {ns/server/\$server/modules}\].\n\n"
            }
            global {
                append out "\[para]\n"
                append out "This module should be loaded globally through \[const ns/modules\].\n\n"
            }
            server {
                append out "\[para]\n"
                append out "This module is loaded per server through \[const {ns/server/\$server/modules}\].\n\n"
            }
            "" {
                # No scope text.
            }
            default {
                error "invalid module scope '$scope'"
            }
        }

        return $out
    }

    :method renderSee {spec see} {
        set links {}

        foreach entry $see {
            set link [:fmtSeeEntry $entry]
            if {$link ne ""} {
                lappend links $link
            }
        }

        if {[llength $links] == 0} {
            return ""
        }

        set plural [expr {[llength $links] > 1}]
        set intro [:metaGet $spec seeIntro ""]
        if {$intro eq ""} {
            set intro [expr {$plural ? {For related documentation, see} : {For detailed documentation, see}}]
        }

        return "\[para]\n$intro [:joinLinks $links].\n\n"
    }

    :method renderParameterList {spec} {
        set out "\[list_begin definitions]\n"

        foreach name [:sortParameterNames [dict keys $spec]] {
            if {[:isMetaKey $name]} {
                continue
            }
            append out [:emitParam $name [dict get $spec $name]]
        }

        append out "\n\[list_end]\n"
        return $out
    }

    :method emitParam {name spec} {
        set out ""
        set hasKey [dict exists $spec key]

        if {$hasKey} {
            set entry [subst {"Parameter name: [:fmtInline [dict get $spec key]]"}]
        } else {
            set entry [subst {"Parameter name: [:fmtEmph $name]"}]
        }
        append out "\n\[def $entry]\n"

        if {[dict exists $spec desc] && [dict get $spec desc] ne ""} {
            append out [:fmtInline [dict get $spec desc]] "\n"
        } else {
            append out "No description available.\n"
        }

        set attrs [:paramAttributes $hasKey $spec]

        if {[llength $attrs] > 0} {
            append out "\n\[list_begin itemized\]\n"
            foreach attr $attrs {
                append out "\[item\] $attr\n"
            }
            append out "\[list_end\]\n"
        }

        return $out
    }

    :method paramAttributes {hasKey spec} {
        set attrs {}

        if {[dict exists $spec type] && [dict get $spec type] ne ""} {
            set keyLabel [expr {$hasKey ? "Value type" : "Type"}]
            lappend attrs "$keyLabel: [:fmtConst [dict get $spec type]]"
        }

        if {[dict exists $spec keySemantics] && [dict get $spec keySemantics] ne ""} {
            switch -- [dict get $spec keySemantics] {
                identifier {set label "configuration-defined identifier"}
                mapping    {set label "mapping key"}
                default    {set label "unknown"}
            }
            lappend attrs "Role of parameter name: $label"
        }

        if {[dict exists $spec values] && [dict get $spec values] ne ""} {
            set values {}
            foreach value [dict get $spec values] {
                lappend values [:fmtConst $value]
            }
            lappend attrs "Allowed values: [join $values {, }]"
        }

        if {[dict exists $spec default]} {
            lappend attrs "Default: [:fmtConst [dict get $spec default]]"
        }

        if {[dict exists $spec cardinality]} {
            switch -- [dict get $spec cardinality] {
                many {
                    lappend attrs {Cardinality: multiple entries with different parameter names are expected}
                }
                multimap {
                    lappend attrs {Cardinality: multiple entries with the same parameter name are expected}
                }
                default {
                    lappend attrs "Cardinality: [:fmtInline [dict get $spec cardinality]]"
                }
            }
        }

        if {[dict exists $spec deprecated]} {
            set deprecated [dict get $spec deprecated]
            if {$deprecated eq ""} {
                lappend attrs "Deprecated."
            } else {
                lappend attrs "Deprecated: [:fmtInline $deprecated]"
            }
        }

        return $attrs
    }

    :method metaGet {spec key {default ""}} {
        set metaKey :$key
        if {[dict exists $spec $metaKey]} {
            return [dict get $spec $metaKey]
        }
        return $default
    }

    :method isMetaKey {key} {
        string match {:*} $key
    }

    :method dedent {text} {
        set lines [split $text \n]

        while {[llength $lines] > 0 && [string trim [lindex $lines 0]] eq ""} {
            set lines [lrange $lines 1 end]
        }
        while {[llength $lines] > 0 && [string trim [lindex $lines end]] eq ""} {
            set lines [lrange $lines 0 end-1]
        }

        set minIndent -1
        foreach line $lines {
            if {[string trim $line] eq ""} {
                continue
            }
            regexp {^[ \t]*} $line indent
            set n [string length $indent]
            if {$minIndent < 0 || $n < $minIndent} {
                set minIndent $n
            }
        }

        if {$minIndent <= 0} {
            return [join $lines \n]
        }

        incr minIndent -1
        set result {}
        foreach line $lines {
            if {[string trim $line] eq ""} {
                lappend result " "
            } else {
                lappend result [string range $line $minIndent end]
            }
        }

        return [join $result \n]
    }

    :method fmtDocBlock {text} {
        set text [string trim [:dedent $text]]
        if {$text eq ""} {
            return ""
        }

        set paragraphs {}
        set current {}

        foreach line [split $text \n] {
            if {[string trim $line] eq ""} {
                if {[llength $current] > 0} {
                    lappend paragraphs [join $current \n]
                    set current {}
                }
            } else {
                lappend current $line
            }
        }

        if {[llength $current] > 0} {
            lappend paragraphs [join $current \n]
        }

        set out ""
        set first 1
        foreach paragraph $paragraphs {
            if {!$first} {
                append out "\[para]\n"
            }
            append out $paragraph "\n\n"
            set first 0
        }

        return $out
    }

    :method fmtSeeEntry {entry} {
        lassign $entry type arg1 arg2

        switch -- $type {
            module {
                set module $arg1
                return "\[uri ../../$module/files/$module.html {$module}]"
            }
            uri {
                if {$arg2 ne ""} {
                    set url   $arg1
                    set label $arg2
                } else {
                    lassign $arg1 url label
                }
                if {$label eq ""} {
                    set label $url
                }
                return "\[uri $url {[:fmtInline $label]}]"
            }
            default {
                puts stderr "warning: unsupported :see entry '$entry'"
                return ""
            }
        }
    }

    :method fmtInline {text} {
        string map {
            \[ "\\["
            \] "\\]"
        } $text
    }

    :method fmtConst {text} {
        return "\[const \"[:fmtInline $text]\"\]"
    }

    :method fmtEmph {text} {
        return "\[emph \"[:fmtInline $text]\"\]"
    }

    :method joinLinks {links} {
        switch [llength $links] {
            0 {return ""}
            1 {return [lindex $links 0]}
            2 {return "[lindex $links 0] and [lindex $links 1]"}
            default {
                set head [lrange $links 0 end-1]
                set last [lindex $links end]
                return "[join $head {, }], and $last"
            }
        }
    }

    :method sortParameterNames {names} {
        set ordinary {}
        set wildcard {}

        foreach name $names {
            if {$name eq "*"} {
                lappend wildcard $name
            } else {
                lappend ordinary $name
            }
        }

        return [concat [lsort -dictionary $ordinary] $wildcard]
    }

    :method writeFile {content} {
        set dir [file dirname ${:outputFile}]
        if {$dir ne "."} {
            file mkdir $dir
        }

        set f [open ${:outputFile} w]
        try {
            puts -nonewline $f $content
        } finally {
            close $f
        }
    }
}

::ns_configdoc::ConfigParamGenerator create ::ns_configdoc::configParamGenerator
::ns_configdoc::configParamGenerator run $argv
