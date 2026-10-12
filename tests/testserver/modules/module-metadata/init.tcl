# Metadata registration takes place through the real Tcl module loader.
ns_module version 1.2.3
ns_module tag test-[ns_info server]

set checks {}
foreach field {version tag} {
    set value [ns_module $field]
    lappend checks [catch {ns_module $field $value} message] $message
}
nsv_set module-metadata redefinition $checks
nsv_set module-metadata empty [catch {ns_module tag ""}]

# C callback metadata cannot be replaced from Tcl, including identical values.
set original [ns_module name]
ns_module name nslog
set checks {}
foreach field {version tag} {
    set value [ns_module $field]
    lappend checks [catch {ns_module $field $value} message] $message
}
nsv_set module-metadata native $checks
ns_module name $original
unset original checks field value message
