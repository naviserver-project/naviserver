
#---------------------------------------------------------------------
# Server's DB configuration -- core module "nsdb"
#---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    ns_param nsdb    nsdb
}
ns_section ns/server/$server/db {
    ns_param pools       pool1,pool2,pool3
    ns_param defaultpool pool1
}


