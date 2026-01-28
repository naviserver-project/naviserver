#---------------------------------------------------------------------
# CGI interface -- core module "nscgi" (optional)
# Enable via:
#   set servermodules { ... nscgi ... }
#---------------------------------------------------------------------
if {"nscgi" in $servermodules} {
    ns_section ns/server/$server/modules {
        ns_param	nscgi nscgi
    }
    ns_section ns/server/$server/module/nscgi {
        ns_param  map	"GET  /cgi-bin ${serverroot}/cgi-bin"
        ns_param  map	"POST /cgi-bin ${serverroot}/cgi-bin"
        ns_param  Interps CGIinterps
        ns_param  allowstaticresources true    ;# default: false
    }
    ns_section ns/interps/CGIinterps {
        ns_param .pl "/usr/bin/perl"
    }
}
