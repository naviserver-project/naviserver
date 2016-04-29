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

proc ns_getform {{charset ""}}  {

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

        set tmpfile [ns_conn contentfile]
        if { $tmpfile eq "" } {
            #
            # Get the content via memory (indirectly via [ns_conn
            # content], the command [ns_conn form] does this)
            #
            set ::_ns_form [ns_conn form]
            foreach {file} [ns_conn files] {
                set offs [ns_conn fileoffset $file]
                set lens [ns_conn filelength $file]
                set hdrs [ns_conn fileheaders $file]
                foreach off $offs len $lens hdr $hdrs {
                    set fp ""
                    while {$fp eq {}} {
                        set tmpfile [ns_mktemp]
                        set fp [ns_openexcl $tmpfile]
                    }
                    ns_atclose [list file delete $tmpfile]
                    fconfigure $fp -translation binary 
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
            # Get the content via external content file
            #
            set ::_ns_form [ns_set create]
            ns_parseformfile $tmpfile $::_ns_form [ns_set iget [ns_conn headers] content-type]
            ns_atclose [list file delete $tmpfile]
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
#   Path of the temporary file or empty if no file found
#
# Side effects:
#   None.
#

proc ns_getformfile {name} {

    ns_getform

    if {[info exists ::_ns_formfiles($name)]} {
        return $::_ns_formfiles($name)
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
#   server puts the whole request into temporary file, if request
#   was in format multipart/form-data, this proc can be used to split
#   multiple parts
#
# Result: 
#   Parses query parameters and uploaded files, puts name/value
#   pairs into provided ns_set, all files are copied into seperate temp 
#   files and stored as name.tmpfile in the ns_set
#

proc ns_parseformfile { file form contentType } {

    if { [catch { set fp [open $file r] } errmsg] } {
        return
    }

    #
    # Note: Currently there is no parsing performed, when the content
    # type is *www-form-urlencoded.
    #
    if { ![regexp -nocase {boundary=(.*)$} $contentType 1 b] } {
        return
    }

    fconfigure $fp -encoding binary -translation binary
    set boundary "--$b"

    while { ![eof $fp] } {
        # skip past the next boundary line
        if { ![string match $boundary* [string trim [gets $fp]]] } {
            continue
        }

        #
        # Fetch the disposition line and field name.
        #
        set disposition [string trim [gets $fp]]
        if { $disposition eq "" } {
            break
        }

        set disposition [split [encoding convertfrom utf-8 $disposition] \;]
        set name [string trim [lindex [split [lindex $disposition 1] =] 1] \"]

        #
        # Fetch and save any field headers (usually just content-type
        # for files).
        #
        set content_type ""
        
        while { ![eof $fp] } {
            set line [string trim [gets $fp]]
            if { $line eq "" } {
                break
            }
            set header [split [encoding convertfrom utf-8 $line] :]
            set key [string tolower [string trim [lindex $header 0]]]
            set value [string trim [lindex $header 1]]

            if {$key eq "content-type"} {
                #
                # Remember content_type to decide later, if content is
                # binary.
                #
                set content_type $value
            }

            ns_set put $form $name.$key $value
        }

        if { [llength $disposition] == 3 } {
            #
            # Uploaded file -- save the original filename as the value
            #
            set filename [string trim [lindex [split [lindex $disposition 2] =] 1] \"]
            ns_set put $form $name $filename

            #
            # Read lines of data until another boundary is found.
            #
            set start [tell $fp]
            set end $start
            
            while { ![eof $fp] } {
                if { [string match $boundary* [string trim [gets $fp]]] } {
                    break
                }
                set end [tell $fp]
            }
            set length [expr {$end - $start - 2}]

            # Create a temp file for the content, which will be deleted
            # when the connection close.  ns_openexcl can fail, hence why 
            # we keep spinning.

            set tmp ""
            while { $tmp eq "" } {
                set tmpfile [ns_mktemp]
                set tmp [ns_openexcl $tmpfile]
            }

            catch {fconfigure $tmp -encoding binary -translation binary}

            if { $length > 0 } {
                seek $fp $start
                fcopy $fp $tmp -size $length
            }

            close $tmp
            seek $fp $end
            ns_set put $form $name.tmpfile $tmpfile

            if { [ns_conn isconnected] } {
                ns_atclose [list file delete $tmpfile]
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
            seek $fp $start
            
            if {$content_type eq "" || [string match text/* $content_type]} {
                set value [encoding convertfrom utf-8 $value]
            }
            ns_set put $form $name $value
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
#   Returns the content the file name of the tmp file (default) or the
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
            set contentfile [ns_mktemp [ns_config ns/parameters tmpdir]/nsd-XXXXXX]
            set F [open $contentfile w]
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
                set ncontentfile [ns_mktemp [ns_config ns/parameters tmpdir]/nsd-XXXXXX]
                set F [open $contentfile r]
                set N [open $ncontentfile w]
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

