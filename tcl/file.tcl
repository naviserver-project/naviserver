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
# file.tcl --
#
#   Support for .tcl-style dynamic pages.
#

#
# Register the ns_sourceproc handler for .tcl files
# if enabled (default: off).
#

set path ns/server/[ns_info server]
set on [ns_config -set -bool $path enabletclpages off]
#ns_log notice "conf: \[$path\] enabletclpages = $on"

if {$on} {
    nsv_set ns:tclfile errorpage [ns_config -set "${path}/tcl" errorpage]
    foreach {method} {GET HEAD POST} {
        ns_register_tcl $method /*.tcl
    }
    ns_log notice "tcl\[[ns_info server]\]: enabletclpages for {GET HEAD POST} requests"
}

#
# ns_tcl_abort --
#
#   Work-alike ns_adp_abort. To be called from within Tcl pages
#   to suspend further processing without generating error.
#
# Results:
#   None.
#
# Side effects:
#   Throws Tcl error which is being caught by [ns_sourcefile]
#

proc ns_tcl_abort {} {
    error ns_tcl_abort "" NS_TCL_ABORT
}

#
# ns_sourceproc --
#
#   Callback for sourcing Tcl pages.
#
#   Get the contents of a file from the cache or disk
#   and source it. Uses tricks to cache Tcl bytecodes
#   and hope to gain some percent of speed that way.
#
#   After 4.99.17, "ns_sourceproc" is deprecated. One should use
#      ns_register_tcl $method /*.tcl
#   instead of
#      ns_register_proc $method /*.tcl ns_sourceproc"
#
# Results:
#   None.
#
# Side effects:
#   May create new cached content in memory.
#

proc ns_sourceproc {args} {

    #ns_deprecated "ns_register_tcl" "Use ns_register_tcl ... instead of ns_register_proc ... ns_sourceproc"

    set path [ns_url2file [ns_conn url]]
    if {![ns_filestat $path stat]} {
        ns_returnnotfound
        return
    }

    set code [catch {

        # Tcl file signature
        set cookie0 $stat(mtime):$stat(ino):$stat(dev)
        set proc0 [info commands ns:tclcache.$path]

        # Verify file modification time
        if {$proc0 eq "" || [$proc0] ne $cookie0} {
            proc ns:tclcache_$path {} [ns_fileread $path]
            proc ns:tclcache.$path {} "return $cookie0"
        }
        # Run the proc
        ns:tclcache_$path

    } errmsg]

    if {$code == 1 && $::errorCode ne "NS_TCL_ABORT"} {

        # Invalidate proc
        rename ns:tclcache_$path ""
        rename ns:tclcache.$path ""

        # Show custom errropage if defined
        set errp [nsv_get ns:tclfile errorpage]
        if {$errp eq {}} {
            return -code 1 -errorcode $::errorCode -errorinfo $::errorInfo $errmsg
        }
        source $errp
    }
}

# proc ns_fileskipbom args {
#     ns_parseargs {
#         {-keepencoding false}
#         channel
#     } $args

#     #ns_log notice "ns_fileskipbom channel $channel keepencoding $keepencoding"
#     set start [tell $channel]
#     set startBytes [read $channel 3]
#     binary scan $startBytes H* hex
#     if {$hex eq "efbbbf"} {
#         #
#         # UTF-8 BOM was found, set end-of-BOM to start of
#         # content.
#         #
#         #ns_log notice "*************** BOM"
#         if {!$keepencoding} {
#             #ns_log notice "*************** BOM reconfigure"
#             fconfigure $channel -encoding utf-8
#         }
#     } else {
#         #
#         # no BOM, reset file pointer to start
#         #
#         seek $channel $start
#     }
# }


# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
