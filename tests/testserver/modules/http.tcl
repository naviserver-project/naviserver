#
# The contents of this file are subject to the AOLserver Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://aolserver.com/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# Copyright (C) 2005 Stephen Deasey <sdeasey@users.sf.net>
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
# Fetch a page from the server, setting any special headers and returning
# the requested parts of the response.
#
proc nstest_http {args} {
    ns_parseargs {-setheaders -getheaders {-getbody 0} -- method location {body ""}} $args

    set reqhdrs [ns_set create]
    if {[info exists setheaders]} {
        foreach {k v} $setheaders {
            ns_set put $reqhdrs $k $v
        }
    }
    set httpid [ns_http queue $method $location $body $reqhdrs]

    set resphdrs [ns_set create]
    ns_http wait $httpid result 10 $resphdrs
    set response [lindex [split [ns_set name $resphdrs]] 1]
    if {[info exists getheaders]} {
        foreach h $getheaders {
            lappend response [ns_set iget $resphdrs $h]
        }
    }
    if {[string is true $getbody]} {
        lappend response $result
    }
    ns_http cleanup

    return $response
}
