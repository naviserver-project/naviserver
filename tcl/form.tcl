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
# form.tcl -- Handle url-encoded or multi-part forms.
#
# Multi-part forms are described in RFC 1867:
#
#   http://www.ietf.org/rfc/rfc1867.txt
#
# Briefly, use:
#
#   <form enctype="multipart/form-data" action="url" method=post>
#   First file: <input name="file1" type="file">
#   Second file: <input name="file2" type="file">
#   <input type="submit">
#   </form>
#
# and then access with:
#
#   set tmpfile1 [ns_getformfile file1]
#   set tmpfile2 [ns_getformfile file2]
#   set fp1 [open $tmpfile1]
#   set fp2 [open $tmpfile2]
#
# Temp files created by ns_getform are removed when the connection closes.
#


#
# ns_queryget --
#
#   Get a value from the http form.
#
# Results:
#   Value for the given key or empty if no form found.
#
# Side effects:
#   May cache current form.
#

proc ns_queryget {key {value ""}}  {

    set form [ns_getform]

    if {$form ne {}} {
        set tmp [ns_set iget $form $key]
        if {[string length $tmp]} {
            set value $tmp
        }
    }

    return $value
}


#
# ns_querygetall --
#
#   Get all values of the same key name from the http form.
#
# Results:
#   Values of the key or def_result if no value found.
#
# Side effects:
#   May cache current form.
#

proc ns_querygetall {key {def_result ""}} {

    set form [ns_getform]

    if {$form eq {}} {
        set result $def_result
    } else {
        set result {}
        set size [ns_set size $form]
        set lkey [string tolower $key]

        #
        # Loop over all keys in the formdata, find all that
        # case-insensitively match the passed-in key and
        # append the values to the return list.
        #

        for {set i 0} {$i < $size} {incr i} {
            set k [ns_set key $form $i]
            if {[string tolower $k] == $lkey} {
                if {[string length [ns_set value $form $i]]} {
                    lappend result [ns_set value $form $i]
                }
            }
        }
        if {$result eq {}} {
            set result $def_result
        }
    }

    return $result
}


#
# ns_queryexists --
#
#   Check if a form key exists.
#
# Results:
#   True of the key exists or false if not.
#
# Side effects:
#   May cache current form.
#

proc ns_queryexists {key} {

    set form [ns_getform]
    set i -1

    if {$form ne {}} {
        set i [ns_set ifind $form $key]
    }

    return [expr {$i >= 0}]
}


#
# ns_getform --
#
#   Return the connection form, copying multipart form data
#   into temp files if necessary.
#
# Results:
#   A set with form key/value pairs or empty if no form found
#
# Side effects:
#   May create number of temporary files for multipart-form-data
#   forms containing data from uploaded files. Also registers
#   an [ns_atclose] callback to delete those file on conn close.
#

proc ns_getform {args}  {
    ns_parseargs {
        {-fallbackcharset ""}
        {charset ""}
    } $args

    if {![ns_conn isconnected]} {
        return
    }

    #
    # If a charset has been specified, use ns_urlcharset
    # to alter the current conn's urlcharset.
    # This can cause cached formsets to get flushed.
    #

    if {$charset ne {}} {
        ns_urlcharset $charset
    }

    #
    # This depends on the fact that global variables
    # in the interpreter are cleaned up on connection
    # close by the [ns_cleanup] command.
    #
    # Also, form caching over the global Tcl variable
    # is not needed any as all is done on C-level.
    #

    if {![info exists ::_ns_form]} {

        set ::_ns_form [ns_conn form -fallbackcharset $fallbackcharset]
        foreach name [ns_set keys $::_ns_form] {
            if {[string match "*.tmpfile" $name]} {
                ns_log warning "Someone tries to sneak-in a fake upload file " \
                    "'$name' value '[ns_set get $::_ns_form $name]': [ns_conn url]"
                ns_set delkey $::_ns_form $name
            }
        }

        set tmpfile [ns_conn contentfile]
        if { $tmpfile eq "" } {
            #
            # Get the content via memory (indirectly via [ns_conn
            # content], the command [ns_conn form] does this)
            #
            ns_log debug "ns_getfrom: get content from memory (files [ns_conn files])"
            foreach {file} [ns_conn files] {
                set offs [ns_conn fileoffset $file]
                set lens [ns_conn filelength $file]
                set hdrs [ns_conn fileheaders $file]
                foreach off $offs len $lens hdr $hdrs {

                    set fp [ns_opentmpfile tmpfile]
                    #set nocomplain [expr {$::tcl_version < 9.0 ? "" : "-profile tcl8"}]
                    set nocomplain "" ;# Tcl9 is a moving target, not sure yet, how this will end up when released
                    try {
                        fconfigure $fp {*}$nocomplain -encoding binary -translation binary
                    } on error {errorMsg} {
                        ns_log warning "ns_getform: fconfigure of temporary file returned: $errorMsg"
                    }

                    ns_atclose [list file delete -- $tmpfile]
                    ns_conn copy $off $len $fp
                    close $fp

                    lappend ::_ns_formfiles($file) $tmpfile
                    set type [ns_set get $hdr content-type]
                    ns_set put $::_ns_form $file.content-type $type
                    # NB: Insecure, access via ns_getformfile.
                    ns_set put $::_ns_form $file.tmpfile $tmpfile
                }
            }
        } else {
            #
            # Get the content via external spool file
            #
            ns_log debug "ns_getfrom: get content from file"
            try {
                #
                # We have to provide a fallback charset here,
                # otherwise, ns_parseformfile would fail, and we
                # would not be able to query the "_charset_" field.
                #
                if {$fallbackcharset eq ""} {
                    set fallbackcharset iso8859-1
                }
                ns_parseformfile \
                    -fallbackcharset $fallbackcharset \
                    $tmpfile \
                    $::_ns_form \
                    [ns_set iget [ns_conn headers] content-type]
            } on error {errorMsg errorDict} {
                #ns_log notice "ns_parseformfile: error in first round, set = [ns_set array $::_ns_form]"
            } on ok {result} {
                set errorDict ""
            }
            set defaultCharset [ns_set get $::_ns_form "_charset_" ""]
            #ns_log notice "ns_parseformfile: call OK -> _charset_ '$defaultCharset'"

            if {$defaultCharset ne ""}  {
                ns_set truncate $::_ns_form 0
                #ns_log notice "ns_parseformfile: second round with charset <$defaultCharset>"
                ns_parseformfile \
                    -encoding [ns_encodingforcharset $defaultCharset] \
                    -fallbackcharset $fallbackcharset \
                    $tmpfile \
                    $::_ns_form \
                    [ns_set iget [ns_conn headers] content-type]
            } elseif {$errorDict ne ""} {
                #
                # There was no "_charset_" specified, throw the error
                # caught above.
                #
                throw [dict get $errorDict -errorcode] $errorMsg
            }
            ns_atclose [list file delete -- $tmpfile]
        }
    }

    return $::_ns_form
}


#
# ns_getformfile --
#
#   Return a tempfile for a form file field.
#
# Result:
#   Path of the temporary file or empty if no file found.  When the
#   INPUT element of the file contains the HTML5 attribute "multiple",
#   a list of filenames is potentially returned.
#
# Side effects:
#   None.
#

if {[info command lmap] eq {}} {
    proc lmap {vars list body} {
        set _r {}
        set _l {}
        foreach _v $vars {
            set _n v[incr _x]
            lappend _l $_n
            upvar 1 $_v $_n
        }
        foreach $_l $list {
            lappend _r [uplevel 1 $body]
        }
        return $_r
    }
}

proc ns_getformfile {name} {

    set form [ns_getform]
    if {[ns_set find $form $name.tmpfile] > -1} {
        return [lmap {k v} [ns_set array $form] {
            if {$k ne "$name.tmpfile"} continue
            set v
        }]
    }
}


#
# ns_openexcl --
#
#   Open a file with exclusive rights. This call will fail if
#   the file already exists in which case "" is returned.
#
# Results:
#   Path of the temporary file or empty if unable to create one.
#
# Side effects:
#   Will attempt unlimited number of times to create new file.
#   This might potentially last long time.
#

proc ns_openexcl {file} {

    if {[catch {set fp [open $file {RDWR CREAT EXCL}]} err]} {
        if {[lindex $::errorCode 1] ne "EEXIST"} {
            return -code error $err
        }
        return
    }

    return $fp
}

#
# For users of Tcl 8.5, the following should be sufficiently
# equivalent. Not sure, we have to support still Tcl 8.5.
#
#proc ns_opentmpfile {varFilename {template ""} {
#    upvar $varFilename tmpFileName
#    set tmpFileName [ns_mktemp {*}$template]
#    set fp [ns_openexcl $tmpFileName]
#}

proc ns_opentmpfile {varFilename {template ""}} {
    upvar $varFilename tmpFileName
    if {$template eq ""} {
        set template [ns_config ns/parameters tmpdir]/nsd-XXXXXX
    }
    return [::file tempfile tmpFileName {*}$template]
}

#
# ns_resetcachedform --
#
#   Reset the http form set currently cached (if any),
#   optionally to be replaced by the given form set.
#
# Results:
#   None.
#
# Side effects:
#   This procedure is deprecated as connection forms
#   are already cached on the C-level
#

proc ns_resetcachedform {{newform ""}} {
    ns_deprecated "" "Forms are cached on the C level."

    if {[info exists ::_ns_form]} {
        unset ::_ns_form
    }
    if {$newform ne {}} {
        set ::_ns_form $newform
    }
}


#
# ns_isformcached --
#
#   Predicate function to answer whether there is
#   a http form set currently cached.
#
# Result:
#   True of form is already cached, false otherwise.
#
# Side effects:
#   This procedure is deprecated as connection forms
#   are already cached on the C-level
#

proc ns_isformcached {} {
    ns_deprecated "" "Forms are cached on the C level."

    return [info exists ::_ns_form]
}


#
# ns_parseformfile --
#
#   Parse a multi-part form data file, this proc does the same
#   thing what internal server does for request content. Primary
#   purpose of this proc to be used with spooled content, when
#   server puts the whole request into temporary file. The proc handles
#   just multipart/form-data and *www-form-urlencoded.
#
# Result:
#   Parses query parameters and uploaded files, puts name/value
#   pairs into provided ns_set, all files are copied into separate temp
#   files and stored as name.tmpfile in the ns_set
#

proc ns_parseformfile {args} {
    ns_parseargs {
        {-fallbackcharset ""}
        {-encoding ""}
        file form contentType
    } $args

    if { [catch { set fp [open $file r] } errmsg] } {
        ns_log warning "ns_parseformfile could not open $file for reading"
    }
    #
    # Separate content-type and options
    #
    set options ""
    regexp {^(.*)\s*;(.*)$} $contentType . contentType options
    #
    # Handle charset unless we have and explicit encoding from the caller
    #
    if {$encoding eq ""} {
        set mimetypeEncoding ""
        if {[regexp {charset\s*=\s*(\S+)} $options . mimetypeCharset]} {
            set mimetypeEncoding [ns_encodingforcharset $mimetypeCharset]
        }
        if {$mimetypeEncoding ne ""} {
            set encoding $mimetypeEncoding
        } else {
            set encoding [ns_conn urlencoding]
        }
    }

    if {[string match "*www-form-urlencoded" $contentType]} {
        #
        # Handle content type application/x-www-form-urlencoded (and
        # similar for strange browsers). We revert here to in-memory
        # parsing for content that is probably not really huge. Also
        # writing a file-based decoder for www-form-urlencoded that
        # sets the contents to the form would require the held the
        # final result in memory.
        #
        try {
            set content [read $fp]
            #ns_log warning "===== ns_parseformfile reads $file $form $contentType -> [string length $content] bytes"
            set s [ns_parsequery -charset $encoding -fallbackcharset $fallbackcharset $content]
            foreach {name value} [ns_set array $s] {
                if {[string match "*.tmpfile" $name]} {
                    ns_log warning "Someone tries to sneak-in a fake upload file " \
                        "'$name' value '$value': [ns_conn url]"
                } else {
                    ns_set put $form $name $value
                }
            }
        } finally {
            close $fp
        }
        return
    }

    #
    # Note: Currently there is no parsing performed, when the content
    # type is neither *www-form-urlencoded nor it has boundaries
    # defined (multipart/form-data).
    #
    if {![regexp -nocase {boundary=(.*)$} $options . b] } {
        #ns_log warning "ns_parseformfile skips form processing: content-type '$contentType' options '$options'"
        close $fp
        return
    }

    #
    # Everything below is just for content-type "multipart/form-data"
    #
    fconfigure $fp -encoding binary -translation binary
    set boundary "--$b"

    #ns_log notice "PARSE multipart inputfile $fp [fconfigure $fp -encoding]"

    while { ![eof $fp] } {
        #
        # Parse part of a multipart entry, containing a boundary line,
        # a header and the body.
        #
        set raw [gets $fp]
        #ns_log notice "PARSE multipart raw boundary <$raw> match [string match $boundary* [string trim $raw]]"
        if { ![string match $boundary* [string trim $raw]] } {
            continue
        }

        set header_set [ns_set create part_header]

        while { ![eof $fp] } {
            set raw [gets $fp]
            #ns_log notice "PARSE multipart raw header <$raw>"
            if {![ns_valid_utf8 $raw]} {
                ns_log warning "multipart header contains invalid UTF-8: $raw"
                close $fp
                throw NS_INVALID_UTF8 "multipart header contains invalid UTF-8"
            }
            set line [string trimright [encoding convertfrom utf-8 $raw] "\r\n"]
            #ns_log notice "PARSE multipart <$line> after trim"

            if { $line eq "" } {
                #
                # Part header finished
                #
                #ns_log notice "PARSE multipart break [eof $fp]"
                break
            }
            #
            # Parse header line (or header continuation line)
            #
            ns_parseheader $header_set $line
        }
        if {[eof $fp]} {
            break
        }

        #
        # Check, if everything necessary was included in the header.
        #
        set content_disposition [ns_set iget $header_set "content-disposition"]
        if {$content_disposition ne ""} {
            #
            # Parse the content of the disposition header into a dict and
            # get field name and filename.
            #
            set disp [lindex [ns_parsefieldvalue $content_disposition] 0]
            set filename [expr {[dict exist $disp filename] ? [dict get $disp filename] : ""}]
            set name     [expr {[dict exist $disp name] ? [dict get $disp name] : ""}]
            #ns_log notice "PARSE multipart extracted filename <$filename> name <$name>"
        } else {
            set name ""
            set filename ""
            ns_log warning "PARSE multipart no content_disposition"
            continue
        }

        set content_type [ns_set iget $header_set "content-type"]
        if {$content_type ne ""} {
            ns_set put $form $name.content-type $content_type
        }

        ns_set free $header_set

        #
        # In case, we got a filename, there will be an upload file,
        # otherwise it will be an ordinary field, where the value is
        # simply put into the ns_set.
        #
        if { $filename ne "" } {
            #
            # Uploaded file -- save the original filename as the value
            #
            ns_set put $form $name $filename

            #ns_fileskipbom -keepencoding true $fp

            #
            # Get file range for the target file (start ... end).
            #
            set start [tell $fp]
            set end $start

            #
            # Read lines of data until another boundary is found.
            #
            while { ![eof $fp] } {
                if { [string match $boundary* [string trim [gets $fp]]] } {
                    break
                }
                set end [tell $fp]
            }
            set length [expr {$end - $start - 2}]

            # Create a temp file for the content, which will be
            # deleted when the connection close.
            #
            # Note that so far the output is written always in
            # binary, no matter what the embedded file-type is.

            set tmp [ns_opentmpfile tmpfile]
            catch {fconfigure $tmp -encoding binary -translation binary}

            if { $length > 0 } {
                seek $fp $start
                #ns_log notice "PARSE multipart fcopy $fp [fconfigure $fp -encoding]" \
                    "-> $tmp [fconfigure $tmp -encoding]"

                fcopy $fp $tmp -size $length
            }

            close $tmp
            seek $fp $end
            ns_set put $form $name.tmpfile $tmpfile

            if { [ns_conn isconnected] } {
                ns_atclose [list file delete -- $tmpfile]
            }

        } else {
            #
            # Ordinary field - read lines until next boundary
            #
            set first 1
            set value ""
            set start [tell $fp]

            while { [gets $fp line] >= 0 } {
                set line [string trimright $line \r]
                if { [string match $boundary* $line] } {
                    break
                }
                if { $first } {
                    set first 0
                } else {
                    append value \n
                }
                append value $line
                set start [tell $fp]
            }
            #ns_log notice "PARSE multipart GOT value <$value>"
            seek $fp $start

            #
            # For ordinary values, newer HTTP specs mandate that
            # the content_type must be omitted, but this was not
            # always so.
            #
            if {$content_type eq "" || [string match "text/*" $content_type]} {
                if {$encoding eq "utf-8" && ![ns_valid_utf8 $value errorString]} {
                    ns_log warning "multipart value for $name contains invalid UTF-8: '$errorString' // $encoding"
                    close $fp
                    throw NS_INVALID_UTF8 "multipart value for $name contains invalid UTF-8"
                }
                set value [encoding convertfrom $encoding $value]
            }
            if {[string match "*.tmpfile" $name]} {
                ns_log warning "Someone tries to sneak-in a fake upload file "\
                    "'$name' value '$value': [ns_conn url]"
            } else {
                ns_set put $form $name $value
            }
        }
    }
    close $fp
}

#
# ns_getcontent --
#
#   Return the content of a request as file or as string, no matter,
#   whether it was spooled during upload into a file or not. The user
#   can specify, whether the result should treated as binary or not.
#   The default is "-as_file true", since this will not run into
#   memory problems on huge files.
#
# Result:
#   Returns the content the filename of the temporary file (default) or the
#   content of the file (when as_file is false).
#

proc ns_getcontent {args} {
    ns_parseargs {
        {-as_file true}
        {-binary true}
    } $args

    if {![string is boolean -strict $as_file]} {
        return -code error "value of '$as_file' is not boolean"
    }
    if {![string is boolean -strict $binary]} {
        return -code error "value of '$binary' is not boolean"
    }

    set contentfile [ns_conn contentfile]
    if {$as_file} {
        #
        # Return the result as a file
        #
        if {$contentfile eq ""} {
            #
            # There is no content file, we have to create it and write
            # the content from [ns_conn content] into it.
            #
            set F [ns_opentmpfile contentfile [ns_config ns/parameters tmpdir]/nsd-XXXXXX]
            if {$binary} {
                fconfigure $F -translation binary
                puts -nonewline $F [ns_conn content -binary]
            } else {
                puts -nonewline $F [ns_conn content]
            }
            close $F
        } else {
            #
            # We have already a content file
            #
            if {!$binary} {
                #
                # We have binary content but want text to be readable,
                # so we have to recode. We use here as well utf-8
                # (like in ns_parseformfile), maybe this has to be
                # parameterized in the future.
                #
                set F [open $contentfile r]
                set N [ns_opentmpfile ncontentfile [ns_config ns/parameters tmpdir]/nsd-XXXXXX]
                fconfigure $F -translation binary
                fconfigure $N -encoding utf-8
                while {1} {
                    set c [read $F 64000]
                    puts -nonewline $N $c
                    if {[eof $F]} break
                }
                close $F
                close $N
                #
                # We cannot delete the old contentfile, since maybe
                # some other part of the code might require it as well
                # via [ns_conn contentfile]
                #
                set contentfile $ncontentfile
            }
        }
        set result $contentfile
    } else {
        #
        # Return the result as a string. Note that in cases, where the
        # file is huge, this might bloat the memory or crash (running
        # in the current max 2 GB limit on Tcl).
        #
        if {$contentfile eq ""} {
            if {$binary} {
                set result [ns_conn content -binary]
            } else {
                set result [ns_conn content]
            }
        } else {
            set F [open $contentfile r]
            if {$binary} {
                fconfigure $F -translation binary
            }
            set result [read $F]
            close $F
        }
    }
    return $result
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
