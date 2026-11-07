# Startup-only: these exclusions belong to interpreter-blueprint setup.
# Keep them with bootstrap initialization rather than runtime API reload.

# exclude the 8.6 TclOO namespace from blueprint creation for now,
# as we don't yet know how to recreate all the artifacts

nstrace::excludensp ::oo
nstrace::excludensp ::oo::*
