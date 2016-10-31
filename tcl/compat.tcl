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
# compat.tcl --
#
#   Procs for backwards compatibility.
#
#

proc ns_deprecated {{alternative ""} {explanation ""}} {
    set msg "[uplevel {info level 0}] is deprecated."
    if {$alternative ne ""} {append msg " Use '$alternative' instead!"}
    if {$explanation ne ""} {append msg " " $explanation}
    ns_log Warning $msg
}

#
# ns_getchannels --
#
#   Returns all opened channels.
#
# Results:
#   List of opened channel handles.
#
# Side effects:
#   None.
#

proc ns_getchannels {} {
    ns_deprecated "file channels"
    file channels
}

#
# ns_cpfp --
#
#   Copies ncopy bytes from input to output channel.
#
# Results:
#   Number of bytes copied.
#
# Side effects:
#   None.
#

proc ns_cpfp {chanin chanout {ncopy -1}} {
    ns_deprecated "fcopy"
    fcopy $chanin $chanout -size $ncopy
}


#
# ns_cp --
#
#   Copies srcfile to dstfile.
#   Syntax: ns_cp ?-preserve? srcfile dstfile
#
# Results:
#   None.
#
# Side effects:
#   Assures that the dstfile has the same modification,
#   access time and attributes as the srcfile if the 
#   optional "-preserve" argument is given.
#

proc ns_cp {args} {
    ns_deprecated "file copy"
    set nargs [llength $args]
    if {$nargs == 2} {
        set pre 0
        set src [lindex $args 0]
        set dst [lindex $args 1]
    } elseif {$nargs == 3 && [string match "-pre*" [lindex $args 0]]} {
        set pre 1
        set src [lindex $args 1]
        set dst [lindex $args 2]
    } else {
        error "wrong # args: should be \"ns_cp ?-preserve? srcfile dstfile\""
    }
    file copy -force -- $src $dst
    if {$pre} {
        file stat $src sbuf
        file mtime $dst $sbuf(mtime)
        file atime $dst $sbuf(atime)
        eval file attributes [list $dst] [file attributes $src]
    }
}


#
# ns_mkdir --
#
#   Creates a directory.
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_mkdir {dir} {
    ns_deprecated "file mkdir"
    file mkdir $dir
}


#
# ns_rmdir --
#
#   Deletes a directory, complaining if the passed path does not
#   point to an empty directory.
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_rmdir {dir} {
    ns_deprecated "file delete"
    if {![file isdirectory $dir]} {
        error "error deleting \"$dir\": not a directory"
    }
    file delete $dir
}


#
# ns_unlink --
#
#   Deletes a file, optionaly complaining if the file is missing.
#   It always complains if the passed path points to a directory.
#
#   Syntax: ns_unlink ?-nocomplain? file
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_unlink {args} {
    ns_deprecated "file delete"
    set nargs [llength $args]
    if {$nargs == 1} {
        set complain 1
        set filepath [lindex $args 0]
    } elseif {$nargs == 2 && [string match "-no*" [lindex $args 0]]} {
        set complain 0
        set filepath [lindex $args 1]
    } else {
        error "wrong # args: should be \"ns_unlink ?-nocomplain? file\""
    }
    if {[file isdirectory $filepath]} {
        error "error deleting \"$filepath\": file is a directory"
    }
    if {$complain && ![file exists $filepath]} {
        error "error deleting \"$filepath\": no such file"
    }
    file delete $filepath
}


#
# ns_link --
#
#   Hard-link the path to a link, eventually complaining.
#   Syntax: ns_link ?-nocomplain? path link
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_link {args} {
    ns_deprecated "file link -hard ..."
    set nargs [llength $args]
    if {$nargs == 2} {
        set cpl 1
        set src [lindex $args 0]
        set lnk [lindex $args 1]
    } elseif {$nargs == 3 && [string match "-no*" [lindex $args 0]]} {
        set cpl 0
        set src [lindex $args 1]
        set lnk [lindex $args 2]
    } else {
        error "wrong # args: should be \"ns_link ?-nocomplain? path link\""
    }
    if {$cpl} {
        file link -hard $lnk $src
    } else {
        catch {file link -hard $lnk $src}
    }

    return
}

#
# ns_rename --
#
#   As we are re-implementing the ns_rename (which actually calls rename())
#   with Tcl [file], lets spend couple of words on the compatibility...
#
#   This is what "man 2 rename" says (among other things):
#
#     The rename() causes the link named "from" to be renamed as "to".  
#     If "to" exists, it is first removed. 
#     Both "from" and "to" must be of the same type (that is, both dirs
#     or both non-dirs), and must reside on the same file system.
#
#   What we cannot guarantee is:
#
#       "must reside on the same file system"
#
#   This is because there is no portable means in Tcl to assure this
#   and because Tcl [file rename] is clever enough to copy-then-delete
#   when renaming files residing on different filesystems.
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_rename {from to} {
    ns_deprecated "file rename"
    if {[file exists $to]} {
        if {[file type $from] != [file type $to]} {
            error "rename (\"$from\", \"$to\"): not of the same type" 
        } elseif {$from == $to} {
            error "error renaming \"$from\": file already exists"
        }
        file delete $to
    }
    file rename $from $to
}


#
# ns_chmod --
#
#   Sets permissions mask of the "file" to "mode".
#
# Results:
#   None.
#
# Side effects:
#   None.
#

proc ns_chmod {file mode} {
    ns_deprecated "file attributes"
    file attributes $file -permissions $mode
}


#
# ns_truncate --
#
#   This is still implement in the server code. The reason is that
#   the Tcl has no portable equivalent; nsd/tclfile.c:NsTclFTruncateObjCmd()
#

#
# ns_ftruncate --
#
#   This is still implement in the server code. The reason is that
#   the Tcl has no portable equivalent; nsd/tclfile.c:NsTclTruncateObjCmd()
#

#
# ns_mktemp --
#
#   This is still implement in the server code. The reason is that
#   the Tcl has no portable equivalent; nsd/tclfile.c:NsTclMkTempObjCmd()
#

#
# ns_tempnam --
#
#   This is still implement in the server code. The reason is that
#   the Tcl has no portable equivalent; nsd/tclfile.c:NsTclTempNamObjCmd()
#

#
# ns_symlink --
#
#   This is still implement in the server code. The reason is that
#   the Tcl [file link] command always creates link target with 
#   absolute path to the linked file; nsd/tclfile.c:NsTclSymlinkObjCmd()
#

#
# ns_adp_compress --
#
#   See: ns_conn
#

proc ns_adp_compress {{bool 1}} {
    ns_deprecated "ns_conn compress"
    ns_conn compress $bool
    return ""
}

#
# ns_adp_stream --
#
#   See: ns_adp_ctl, ns_adp_flush.
#

proc ns_adp_stream {{bool 1}} {
    ns_deprecated "ns_adp_ctl stream"
    ns_adp_ctl stream $bool
    ns_adp_flush
}

#
# ns_unregister_proc --
#
#   ns_unregister_op unregisters any kind of registered
#   request, including C, Tcl, ADP etc.
#

proc ns_unregister_proc {args} {
    ns_deprecated "ns_unregister_op"
    uplevel [list ns_unregister_op {*}$args]
}

#
# ns_var --
#
#   Tcl shared variables. Use nsv_* instead.
#
proc ns_var {cmd {key ""} {value ""}} {
    ns_deprecated "nsv_*"
    switch $cmd {

        exists { return [nsv_exists ns:var $key] }
        list   { return [nsv_list   ns:var] }
        get    { return [nsv_get    ns:var $key] }
        set    { return [nsv_set    ns:var $key $value] }
        unset  { return [nsv_unset  ns:var $key] }

        default {
            error "unknown command \"$cmd\", should be exists, list, get, set, or unset"
        }
    }
}

#
# ns_hmac_sha2 --
#
#   compute a HMAC for key and message
#   use "::ns_crypto::hmac string -digest ..." instead
#
proc ns_hmac_sha2 args {
    set length 256
    
    ns_parseargs {
	{-length 256}
	key
	message
    } $args
    
    ns_deprecated "::ns_crypto::hmac string -digest sha$length ..."
    uplevel [list ::ns_crypto::hmac string -digest sha$length $key $message]
}

#
# ns_sha2 --
#
#   compute a SHA2 digest for message
#   use "::ns_crypto::md string -digest ..." instead
#
proc ns_sha2 args {
    set length 256
    
    ns_parseargs {
	{-length 256}
	message
    } $args
    
    ns_deprecated "ns_crypto::md string -digest sha$length ..."
    uplevel [list ns_crypto::md string -digest sha$length $message]
}

#
# ns_tmpnam --
#
#   return a name of a temporary file
#   use "::ns_mktemp" instead
#
proc ns_tmpnam {} {
    ns_deprecated "ns_mktemp"
    return [ns_mktemp]
}


# EOF

