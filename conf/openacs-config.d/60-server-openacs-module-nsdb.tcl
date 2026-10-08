#---------------------------------------------------------------------
# Server's DB configuration -- core module "nsdb"
#---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    ns_param nsdb    nsdb
}

# Pools available through NaviServer's ns_db API.
ns_section ns/server/$server/db {
    ns_param pools       pool1,pool2,pool3
    # ns_param pools     pool1,pool2,pool3,smtp_sqlite
    ns_param defaultpool pool1
}

# Pools used by OpenACS's db_* API.
ns_section ns/server/${server}/acs/database {
    ns_param database_names {main}
    ns_param pools_main     {pool1 pool2 pool3}
}
