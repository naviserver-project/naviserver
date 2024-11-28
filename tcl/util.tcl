#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
#

#
# util.tcl --
#
#   Various utility procedures. Couple of please's in advance:
#
#   o. DO NOT use this file as general purpose sink, if possible
#
#   o. THINK TWICE before naming the utility procedure with "ns_"
#      prefix as this makes it a first class citizen in the API
#      which means we will all have to maintain it for a long time.
#
#   o. DOCUMENT the procedure with a short header so we all know
#      what it is SUPPOSED to do, not what it does.
#      What it does, we can read from the code...
#
#   o. STICK to the indenting rules we all agreed upon already.
#


#
# ns_adp_include --
#
#   Ensure a new call frame with private variables.
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_adp_include {args} {
    _ns_adp_include {*}$args
}


#
# ns_setexpires --
#
#   Assures connection output headers contain the "Expires"
#   header. When "-cache-control" is specified the function adds as
#   well a max-age header field to the response with the specified
#   cache response directive (such as public, private, no-cache,
#   no-store, no-transform, must-revalidate, or proxy-revalidate)
#
# Results:
#   None.
#
# Side effects:
#   Output headers set may be extended.
#

proc ns_setexpires {args} {
    set secs [lindex $args end]
    if {[lindex $args 0] eq "-cache-control"} {
        set cache_control [lindex $args 1]
        ns_set iupdate [ns_conn outputheaders] cache-control "max-age=$secs, [lindex $args 1]"
    } elseif {[llength $args] > 1} {
        error "usage: ns_setexpires ?-cache-control public|private|no-cache|no-store|no-transform|must-revalidate|proxy-revalidate? secs"
    }
    set when [ns_httptime [expr {$secs + [clock seconds]}]]
    ns_set iupdate [ns_conn outputheaders] expires $when
}


#
# ns_fileread --
#
#   Read binary data of the given file.
#
# Results:
#   Binary data from the file.
#
# Side effects:
#   None.
#

proc ns_fileread {filename} {

    set fd [open $filename]
    fconfigure $fd -translation binary
    set data [read $fd]
    close $fd

    return $data
}


#
# ns_filewrite
#
#   Write binary data into the file
#
# Results:
#   None.
#
# Side effects:
#   May create new file on the filesystem.
#

proc ns_filewrite {filename data {mode w}} {

    set fd [open $filename $mode]
    fconfigure $fd -translation binary
    puts -nonewline $fd $data
    close $fd
}


#
# ns_findset --
#
#   Returns a set with a given name from a list of sets
#
# Results:
#   The set ID
#
# Side effects:
#   None.
#

proc ns_findset {sets name} {

    foreach set $sets {
        if {[ns_set name $set] eq $name} {
            return $set
        }
    }
}


proc ns_joinurl {dict} {
    #
    # Inverse operation of ns_parseurl
    #
    set valid_keys {proto userinfo host port path tail query fragment}
    if {!([string is list $dict] && ([llength $dict] & 1) == 0)} {
        error "the provided argument is not a dict: $dict"
    }
    foreach key [dict keys $dict] {
        if {$key ni $valid_keys} {
            error "the provided dict contains invalid key: $key"
        }
    }
    set URL ""
    dict with dict {
        if {[info exists host]} {
            if {[info exists proto]} {
                append URL [expr {$proto ne "" ? "$proto:" : ""}]
            }
            append URL //
            if {[info exists userinfo] && $userinfo ne ""} {
                append URL $userinfo @
            }
            append URL [expr {[string match *:* $host] ? "\[$host\]" : $host}]
            if {[info exists port]} {
                append URL : $port
            }
        } else {
            if {[info exists proto]} {
                error "key 'proto' (value $proto) was provided without a key 'host'"
            } elseif {[info exists userinfo]} {
                error "key 'userinfo' (value $userinfo) was provided without a key 'host'"
            } elseif {[info exists port]} {
                error "key 'port' (value $port) was provided without a key 'host'"
            }
        }
        if {[info exists path]} {
            if { $path ne ""} {
                append URL /
            }
            append URL $path /
        }

        if {[info exists tail] && $tail ne ""} {
            append URL $tail
        }

        if {[info exists query] && $query ne ""} {
            append URL ? $query
        }
        if {[info exists fragment] && $fragment ne ""} {
            append URL # $fragment
        }
    }
    return $URL
}

#=============================================================================
#
#     **** Please reconsider removal of the code below this marker *****
#
#=============================================================================

#
# ns_parsetime --
#

proc ns_parsetime {option time} {
    ns_deprecated "clock format"

    set parts {sec min hour mday mon year wday yday isdst}
    set pos [lsearch -exact $parts $option]

    if {$pos == -1} {
        error "Incorrect option to ns_parsetime: \"$option\" Should be\
               one of \"$parts\""
    }

    return [lindex $time $pos]
}


#
# getformdata --
#
#   Make sure an HTML FORM was sent with the request.
#

proc getformdata {formVar} {
    ns_deprecated "ns_conn form"

    upvar 1 $formVar form

    set form [ns_conn form]
    if {$form eq {}} {
        ns_returnbadrequest "Missing HTML FORM data"
        return 0
    }

    return 1
}


#
# ns_paren --
#
#   deprecated
#

proc ns_paren {val} {
    ns_deprecated "use standard Tcl functions" {one-liner}
    if {$val ne {}} {
        return "($val)"
    }
}


#
# Paren --
#

proc Paren {val} {
    ns_deprecated "ns_paren"
    return [ns_paren $val]
}


#
# issmallint --
#
#   Returns true if passed value is a small integer (16 bits)
#

proc issmallint {val} {
    ns_deprecated "ns_issmallint"
    ns_issmallint $val
}


#
# ns_issmallint --
#
#   Returns true if passed value fits into an unsigned 16-bit integer
#

proc ns_issmallint {value} {
    expr {[string is integer -strict $value]
          && $value <= 65535 && $value >= -65535}
}


#
#  ns_formvalueput --
#
#   deprecated.
#

proc ns_formvalueput {htmlpiece dataname datavalue} {
    ns_deprecated "standard API" {not sufficent for today's needs}

    set newhtml ""

    while {$htmlpiece ne ""} {
        if {[string index $htmlpiece 0] ne "<"} {
            regexp {([^<]*)(.*)} $htmlpiece m brandnew htmlpiece
            append newhtml $brandnew
        } else {
            regexp {<([^>]*)>(.*)} $htmlpiece m tag htmlpiece
            set tag [string trim $tag]
            set CAPTAG [string toupper $tag]
            switch -regexp -- $CAPTAG {
                {^INPUT} {
                    if {[regexp {TYPE=("IMAGE"|"SUBMIT"|"RESET"|IMAGE|SUBMIT|RESET)} $CAPTAG]} {
                        append newhtml <$tag>
                    } elseif {[regexp {TYPE=("CHECKBOX"|CHECKBOX|"RADIO"|RADIO)} $CAPTAG]} {
                        set name [ns_tagelement $tag NAME]
                        if {$name eq $dataname} {
                            set value [ns_tagelement $tag VALUE]
                            regsub -all -nocase { *CHECKED} $tag {} tag
                            if {$value eq $datavalue} {
                                append tag " CHECKED"
                            }
                        }
                        append newhtml <$tag>

                    } else {

                        ## If it is an INPUT TYPE that hasn't been covered
                        #  (text, password, hidden, other (defaults to text))
                        ## then we add/replace the VALUE tag

                        set name [ns_tagelement $tag NAME]
                        if {$name eq $dataname} {
                            ns_tagelementset tag VALUE $datavalue
                        }
                        append newhtml <$tag>
                    }
                }
                {^TEXTAREA} {

                    ###
                    #   Fill in the middle of this tag
                    ###

                    set name [ns_tagelement $tag NAME]
                    if {$name eq $dataname} {
                        while {![regexp -nocase {^<( *)/TEXTAREA} $htmlpiece]} {
                            regexp {^.[^<]*(.*)} $htmlpiece m htmlpiece
                        }
                        append newhtml <$tag>$datavalue
                    } else {
                        append newhtml <$tag>
                    }
                }
                {^SELECT} {

                    ### Set flags "inkeyselect" and "addoption" so
                    ### OPTION and /SELECT know what to look for.

                    if {[ns_tagelement $tag NAME] eq $dataname} {
                        set inkeyselect 1
                        set addoption 1
                    } else {
                        set inkeyselect 0
                        set addoption 0
                    }
                    append newhtml <$tag>
                }
                {^OPTION} {

                    ###
                    #   Find the value for this
                    ###

                    if {$inkeyselect} {
                        regsub -all -nocase { *SELECTED} $tag {} tag
                        set value [ns_tagelement $tag VALUE]
                        regexp {^([^<]*)(.*)} $htmlpiece m txt htmlpiece
                        if {$value eq ""} {
                            set value [string trim $txt]
                        }
                        if {$value eq $datavalue} {
                            append tag " SELECTED"
                            set addoption 0
                        }
                        append newhtml <$tag>$txt
                    } else {
                        append newhtml <$tag>
                    }
                }
                {^/SELECT} {

                    ###
                    #   Do we need to add to the end?
                    ###

                    if {$inkeyselect && $addoption} {
                        append newhtml "<option selected>$datavalue<$tag>"
                    } else {
                        append newhtml <$tag>
                    }
                    set inkeyselect 0
                    set addoption 0
                }
                default {
                    append newhtml <$tag>
                }
            }
        }
    }

    return $newhtml
}


#
# ns_tagelement --
#
#   deprecated
#

proc ns_tagelement {tag key} {
    ns_deprecated "tDOM or similar for updating HTML snippets"

    set qq {"([^\"]*)"}               ; # Matches what's in quotes
    set pp {([^ >]*)}                 ; # Matches a word (mind yer pp and qq)

    if {[regexp -nocase -- "$key *= *$qq" $tag m name]} {
        # Do nothing
    } elseif {[regexp -nocase -- "$key *= *$pp" $tag m name]} {
        # Do nothing
    } else {
        set name ""
    }

    return $name
}


#
# ns_tagelementset --
#
#   deprecated
#
proc ns_tagelementset {tagvar key value} {
    ns_deprecated "tDOM or similar for updating HTML snippets"

    upvar $tagvar tag

    set qq {"([^\"]*)"}                ; # Matches what's in quotes
    set pp {([^ >]*)}                  ; # Matches a word (mind yer pp and qq)

    regsub -all -nocase -- "$key=$qq" $tag {} tag
    regsub -all -nocase -- "$key *= *$pp" $tag {} tag
    append tag " $key=\"$value\""
}


#
# Helper procedure for ns_htmlselect.
# Sorts a list of pairs based on the first value in each pair
#

proc _ns_paircmp {pair1 pair2} {

    if {[lindex $pair1 0] > [lindex $pair2 0]} {
        return 1
    } elseif {[lindex $pair1 0] < [lindex $pair2 0]} {
        return -1
    } else {
        return 0
    }
}


#
# ns_htmlselect --
#
#   ns_htmlselect ?-multi? ?-sort? ?-labels labels? /key/ /values/ ?selecteddata?
#

proc ns_htmlselect args {

    set multi  0
    set sort   0
    set labels {}

    while {[string index [lindex $args 0] 0] eq "-"} {
        if {[lindex $args 0] eq "-multi"} {
            set multi 1
            set args [lreplace $args 0 0]
        }
        if {[lindex $args 0] eq "-sort"} {
            set sort 1
            set args [lreplace $args 0 0]
        }
        if {[lindex $args 0] eq "-labels"} {
            set labels [lindex $args 1]
            set args [lreplace $args 0 1]
        }
    }

    set key [lindex $args 0]
    set values [lindex $args 1]

    if {[llength $args] == 3} {
        set selecteddata [lindex $args 2]
    } else {
        set selecteddata ""
    }

    set select "<select name='$key'"
    if {$multi == 1} {
        set size [llength $values]
        if {$size > 5} {
            set size 5
        }
        append select " multiple size='$size'"
    } else {
        if {[llength $values] > 25} {
            append select " size='5'"
        }
    }
    append select ">\n"
    set len [llength $values]
    set lvpairs {}
    for {set i 0} {$i < $len} {incr i} {
        if {$labels eq ""} {
            set label [lindex $values $i]
        } else {
            set label [lindex $labels $i]
        }
        regsub -all "\"" $label "" label
        lappend lvpairs [list  $label [lindex $values $i]]
    }
    if {$sort} {
        set lvpairs [lsort -command _ns_paircmp -increasing $lvpairs]
    }
    foreach lvpair $lvpairs {
        append select [subst {<option value="[lindex $lvpair 1]"}]
        if {[lindex $lvpair 1] in $selecteddata} {
            append select " selected"
        }
        append select ">[lindex $lvpair 0]\n"
                   }
    append select "</select>"

    return $select
}


#
# ns_browsermatch --
#
#   <deprecated>
#

proc ns_browsermatch {pattern} {
    ns_deprecated "standard API" {one-liner}
    string match $pattern [ns_set iget [ns_conn headers] user-agent] $agnt
}

#
# ns_set_precision --
#

proc ns_set_precision {precision} {
    ns_deprecated {set ::tcl_precision $precision}
    set ::tcl_precision $precision
}


#
# ns_updateheader --
#

proc ns_updateheader {key value} {
    ns_deprecated {ns_set update [ns_conn outputheaders]}
    ns_set update [ns_conn outputheaders] $key $value
}


#
# ns_subnetmatch /cidr/ /ipaddr/ --
#
#   Returns true if ipaddr (IPv4 or IPv6) matches the CDIR (Classless
#   Inter-Domain Routing) range.
#   https://en.wikipedia.org/wiki/Classless_Inter-Domain_Routing
#

proc ns_subnetmatch {cidr ipaddr} {
    ns_deprecated "ns_ip match"
    ns_ip match $cidr $ipaddr
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
