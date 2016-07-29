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
# init.tcl --
#
#    NaviServer looks for init.tcl before sourcing all other files
#    in directory order.
#

#
# Initialize errorCode and errorInfo like tclsh does.
#

set ::errorCode ""
set ::errorInfo ""

#
# Make sure Tcl package loader starts looking for
# packages with our private library directory and not
# in some public, like /usr/local/lib or such. This
# way we avoid clashes with modules having multiple
# versions, one for general use and one for NaviServer.
#

if {[info exists ::auto_path] == 0} {
    set ::auto_path [file join [ns_info home] lib]
} else {
    set ::auto_path [concat [file join [ns_info home] lib] $::auto_path]
}

# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
