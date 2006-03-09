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
# $Header$
#

#
# file.tcl --
#
#   Support for .tcl-style dynamic pages.
#

#
# Register the ns_sourceproc handler for .tcl files
# if enabled (default: off).
#

set path ns/server/[ns_info server]
set on [ns_config -bool $path enabletclpages off]
ns_log notice "conf: \[$path\]enabletclpages = $on"

if {$on} {
    ns_log notice "tcl: enabling .tcl pages"
    nsv_set ns:tclfile errorpage [ns_config "${path}/tcl" errorpage]
    ns_register_proc GET  /*.tcl ns_sourceproc
    ns_register_proc POST /*.tcl ns_sourceproc
    ns_register_proc HEAD /*.tcl ns_sourceproc
    ns_cache_create ns:filecache [ns_config $path filecachesize 5000000]
}


#
# ns_tcl_abort --
#
#   Work-alike ns_adp_abort. To be called from within Tcl pages
#   to suspend futher processing without generating error.
#
# Results:
#   None.
#
# Side effects:
#   Throws Tcl error wich is being caught by [ns_sourcefile]
#

proc ns_tcl_abort {} {
    error ns_tcl_abort "" NS_TCL_ABORT
}


#
# ns_sourcefile --
#
#   Get the contents of a file from the cache or disk
#   and source it. Uses tricks to cache Tcl bytecodes
#   and hope to gain some percent of speed that way.
#
# Results:
#   None.
#
# Side effects:
#   Caches the file content and Tcl_Obj's rep of the
#   sourced script in per-thread cache (is this true?)
#

proc ns_sourcefile {filename} {

    file stat $filename stat
    set current_cookie $stat(mtime):$stat(ctime):$stat(ino):$stat(dev)

    #
    # Read current cached file
    #

    set pair [ns_cache_eval ns:filecache $filename {
        list $current_cookie [ns_fileread $filename]
    }]

    #
    # If changed, re-cache it
    #

    if {[lindex $pair 0] ne $current_cookie} {
        ns_cache_flush ns:filecache $filename
        set pair [ns_cache_eval ns:filecache $filename {
            list $current_cookie [ns_fileread $filename]
        }]
    }

    #
    # And here's the magic part. We're using "for" here to translate the
    # text source file into bytecode, which will be associated with the 
    # Tcl_Obj we just cached (as its internal representation).  "eval"
    # doesn't do this as the eval provided in Tcl uses the TCL_EVAL_DIRECT
    # flag, and hence interprets the text directly.
    #

    uplevel [for [lindex $pair 1] {0} {} {}]
}

#
# ns_sourceproc --
#
#   Callback for sourcing Tcl pages.
#
# Results:
#   None.
#
# Side effects:
#   May create new cached content in memory.
#

proc ns_sourceproc {args} {

    set file [ns_url2file [ns_conn url]]
    if {![file exists $file]} {
        ns_returnnotfound
        return
    }
    set code [catch {ns_sourcefile $file} res]
    if {$code == 1 && $::errorCode eq "NS_TCL_ABORT"} {
        return
    }
    set errp [nsv_get ns:tclfile errorpage]
    if {$errp eq {}} {
        return -code $code -errorcode $::errorCode -errorinfo $::errorInfo $res
    }
    source $errp
}

# EOF $RCSfile$
