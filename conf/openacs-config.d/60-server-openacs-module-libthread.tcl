#---------------------------------------------------------------------
# Tcl Thread library -- extra module "libthread"
# ---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    #
    # Determine, if libthread is installed. First check for a version
    # having the "-ns" suffix. If this does not exist, check for a
    # legacy version without it.
    #
    set libthread \
        [lindex [lsort [glob -nocomplain \
                            $homedir/lib/thread*/libthread-ns*[info sharedlibextension]]] end]
    if {$libthread eq ""} {
        set libthread \
            [lindex [lsort [glob -nocomplain \
                                $homedir/lib/thread*/lib*thread*[info sharedlibextension]]] end]
    }
    if {$libthread eq ""} {
        ns_log notice "No Tcl thread library installed in $homedir/lib/"
    } else {
        ns_param	libthread $libthread
        ns_log notice "Use Tcl thread library $libthread"
    }
}


