#---------------------------------------------------------------------
# WebDAV support (optional; requires oacs-dav)
#---------------------------------------------------------------------
#ns_section ns/server/$server/tdav {
#    ns_param	propdir            $serverroot/data/dav/properties
#    ns_param	lockdir            $serverroot/data/dav/locks
#    ns_param	defaultlocktimeout 300
#}
#
#ns_section ns/server/$server/tdav/shares {
#    ns_param	share1		"OpenACS"
#}
#
#ns_section ns/server/$server/tdav/share/share1 {
#    ns_param	uri		"/dav/*"
#    ns_param	options		"OPTIONS COPY GET PUT MOVE DELETE HEAD MKCOL POST PROPFIND PROPPATCH LOCK UNLOCK"
#}
