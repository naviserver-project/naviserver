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
# charsets.tcl --
#
#   Routines for working with character set encodings
#   for support of internationalized character sets.
#


#
# ns_urlcharset -
#
#    Set the current connections' urlcharset.
#
# Results:
#   Returns the encoding value for the specified charset.
#
# Side effects:
#   If the connection's urlencoding value is being changed
#   then flush any form set cached locally (ns_conn will
#   do the same).
#

proc ns_urlcharset {charset} {

    set encoding [ns_encodingforcharset $charset]

    if {$encoding ne [ns_conn urlencoding]} {
        ns_resetcachedform
        return [ns_conn urlencoding [ns_encodingforcharset $charset]]
    } else {
        return $encoding
    }
}


#
# ns_setformencoding --
#
#   Set the 'form encoding' of the current connection.
#   This turns similar to ns_urlcharset.
#
# Results:
#    None.
#
# Side effects:
#    See ns_urlcharset.
#

proc ns_setformencoding {charset} {
    ns_urlcharset $charset
}


#
# ns_formfieldcharset --
#
#   This function will examine the incoming form for a field with the
#   given name, which is expected to contain the character set that
#   the data is encoded.  If this field is found, use that charset to
#   set the urlencoding for the current connection.
#
#   deprecated
#
# Results:
#    None.
#
# Side effects:
#    Sets the urlencoding for the current connection.
#

proc ns_formfieldcharset {name} {
    ns_deprecated {standard internationalization commands}

    set charset ""

    if {[_ns_multipartformdata_p]} {
        set form [ns_getform]
        if {$form ne {}} {
            set charset [ns_set get $form $name]
            if {$charset ne {}} {
                ns_urlcharset $charset
            }
        }
    } else {
        set query [ns_conn query]
        regexp "&$name=(\[^&]*)" "&$query" null charset
    }
    if {$charset ne {}} {
        ns_urlcharset $charset
    }
}


#
# ns_cookiecharset --
#
#   This function will examine the incoming request for a cookie
#   with the given name, which is expected to contain the
#   character set that the data is encoded.  If this cookie
#   is found, use that charset to set the urlencoding for the
#   current connection.
#
#   deprecated
#
# Results:
#    None.
#
# Side effects:
#    Sets the urlencoding for the current connection.
#

proc ns_cookiecharset {name} {
    ns_deprecated {standard internationalization commands}

    set cookies [split [ns_set iget [ns_conn headers] cookie] ";"]
    set charset ""

    foreach cookie $cookies {
        set cookie [string trim $cookie]
        if {[regexp -- "$name=(.*)" $cookie junk charset]} {
            break
        }
    }

    if {$charset ne {}} {
        ns_urlcharset $charset
    }
}


#
# ns_encodingfortype --
#
#   Parses the given mime-type string to determine the character
#   encoding implied by it.  Will use the configured OutputCharset
#   if no charset is explicitly specified in the given string.
#
# Results:
#    Encoding name
#
# Side effects:
#   None.
#

proc ns_encodingfortype {mimetype} {

    set mimetype [string trim [string tolower $mimetype]]

    if {[regexp {;[ \t\r\n]*charset[ \t\r\n]*=([^;]*)} $mimetype junk set]} {
        return [ns_encodingforcharset [string trim $set]]
    } elseif {[string match "text/*" $mimetype]} {
        set set [ns_config -set ns/parameters OutputCharset iso-8859-1]
        return [ns_encodingforcharset $set]
    } else {
        return binary
    }
}


#
# ns_choosecharset --
#
#   Performs an analysis of the request's accepted charsets, against
#   either the given charset list, or the configured default preferred
#   character set list (ns/parameters/PreferredCharsets).
#
#   Deprecated
#
# Results:
#    One character set name.
#
# Side effects:
#    None.
#

proc ns_choosecharset {args} {
    ns_deprecated {standard internationalization commands}

    set preferred_charsets \
        [ns_config -set ns/parameters PreferredCharsets {utf-8 iso-8859-1}]

    set default_charset \
        [ns_config -set ns/parameters OutputCharset iso-8859-1]

    for {set i 0; set n [llength $args]} {$i < $n} {incr i} {
        set arg [lindex $args $i]
        switch -glob -- $arg {
            -pref* {
                incr i
                if {$i >= $n} {
                    error "Missing argument for $arg"
                }
                set preferred_charsets [lindex $args $i]
            }
            default {
                error "Usage: ns_choosecharset ?-preference charset-list?"
            }
        }
    }

    #
    # Figure out what character sets the client will accept.
    #

    set accept_charset_header \
        [string tolower [ns_set iget [ns_conn headers] accept-charset]]

    regsub -all {[ \t\r\n]} $accept_charset_header {} accept_charset_header

    if {$accept_charset_header eq {}} {

        #
        # No character sets given, send the default.
        #

        return $default_charset
    }

    if {[string first ";q=" $accept_charset_header] == -1} {

        #
        # No q values; just use the header order.
        #

        set accept_charsets [split $accept_charset_header ","]

    } else {

        #
        # At least one q value; sort by q values.
        #

        set list {}
        foreach acharset [split $accept_charset_header ","] {
            set i [string first ";q=" $acharset]
            if {$i == -1} {
                lappend list [list $acharset 1.0]
            } else {
                incr i -1
                set charset [string range $acharset 0 $i]
                incr i 4
                set q [string range $acharset $i end]
                if {![regexp {^[01](\.[0-9]{0,3})?} $q]} {
                    set q 0.0
                } elseif {$q > 1.0} {
                    set q 1.0
                }
                if {$q > 0.0} {
                    lappend list [list $charset $q]
                }
            }
        }
        set accept_charsets {}
        foreach pair [lsort -decreasing -real -index 1 $list] {
            lappend accept_charsets [lindex $pair 0]
        }
    }

    if {[llength $accept_charsets] == 0} {

        #
        # No charsets had a q value > 0.
        #

        return $default_charset
    }

    foreach charset $preferred_charsets {
        if {$charset in $accept_charsets} {

            #
            # Return the first preferred charset
            # that is acceptable to the client.
            #

            return $charset
        }
    }

    set supported_charsets [ns_charsets]

    foreach charset $accept_charsets {
        if {$charset in $supported_charsets} {

            #
            # Return the first acceptable charset
            # that we support.
            #

            return $charset
        }
    }

    return $default_charset
}


#
# _ns_multipartformdata_p --
#
#    Helper to check whether current request has multi-part form data.
#
# Results:
#    True, if form is a multi-part post.
#
# Side effects:
#    None.
#

proc _ns_multipartformdata_p {} {
    ns_deprecated "" "helper of deprecated function"

    set type [string tolower [ns_set iget [ns_conn headers] content-type]]
    set glob {*multipart/form-data*}

    return [expr {[ns_conn method] eq {POST} && [string match $glob $type]}]
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
