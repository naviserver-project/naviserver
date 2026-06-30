# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Gustaf Neumann fecit, June 2026

namespace eval ::ns_privacy {}

proc ::ns_privacy::signals {} {
    set headers [ns_conn headers]

    set result [dict create gpc 0 dnt 0]

    set gpc [ns_set get $headers Sec-GPC]
    if {$gpc eq "1"} {
        dict set result gpc 1
    }

    set dnt [ns_set get $headers DNT]
    if {$dnt eq "1"} {
        dict set result dnt 1
    }

    set adpc [ns_set get $headers ADPC]
    if {$adpc eq ""} {
        dict set result adpc [dict create present 0]
    } else {
        dict set result adpc [::ns_privacy::parse_adpc $adpc]
    }

    return $result
}

proc ::ns_privacy::parse_adpc {value} {
    set result [dict create \
        present 1 \
        valid 1 \
        raw $value \
        consent {} \
        withdraw {} \
        withdraw_all 0 \
        unknown {} \
        errors {}]

    try {
        set fields [ns_parsefieldvalue -lower $value]
        #ns_log notice "<$value> -> <$fields>"
    } on error {errmsg opts} {
        dict set result valid 0
        dict lappend result errors $errmsg
        return $result
    }

    foreach field $fields {
        foreach {name fieldValue} $field {
            switch -- $name {
                consent {
                    foreach id [split $fieldValue] {
                        if {$id ne ""} {
                            dict lappend result consent $id
                        }
                    }
                }

                withdraw {
                    if {$fieldValue eq "*"} {
                        dict set result withdraw_all 1
                    } else {
                        foreach id [split $fieldValue] {
                            if {$id ne ""} {
                                dict lappend result withdraw $id
                            }
                        }
                    }
                }

                default {
                    dict lappend result unknown $name $fieldValue
                }
            }
        }
    }
    #ns_log notice "--> dict <$result>"

    return $result
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
