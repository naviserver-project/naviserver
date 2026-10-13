# Revproxy is shipped with NaviServer; report the running core's build metadata.
# Metadata is optional; loading definitions may have no module context.
catch {
    ns_module version [ns_info patchlevel]
    ns_module tag [ns_info tag]
}
