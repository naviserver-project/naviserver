######################################################################
# Section 4 -- Global database drivers and pools
######################################################################

#---------------------------------------------------------------------
# Database drivers
#
# Make sure the drivers are compiled and installed in $homedir/bin.
# Supported values for $dbms in this template:
#   oracle  -- use the nsoracle driver
#   postgres -- use nsdbpg (PostgreSQL)
#---------------------------------------------------------------------
ns_section ns/db/drivers {
    if { $dbms eq "oracle" } {
        set db_driver_name nsoracle
        ns_param $db_driver_name nsoracle
    } else {
        set db_driver_name postgres
        ns_param $db_driver_name nsdbpg

        # When debugging SQL, you can enable SQL-level debug messages
        # via ns_logctl. The following is an example of a useful runtime
        # setting (only available in the PostgreSQL driver).
        #
        # ns_logctl severity "Debug(sql)" -color blue $verboseSQL
    }
}

#---------------------------------------------------------------------
# Driver-specific settings
#---------------------------------------------------------------------

# Oracle driver-specific settings
ns_section ns/db/driver/nsoracle {
    # Maximum length of SQL strings to log; -1 means no limit.
    ns_param maxStringLogLength -1

    # Buffer size for LOB operations.
    ns_param LobBufferSize      32768
}

# PostgreSQL driver-specific settings
ns_section ns/db/driver/postgres {
    # Set this parameter when "psql" is not on your PATH (OpenACS specific).
    # ns_param pgbin "/usr/lib/postgresql/16/bin/"
}

#---------------------------------------------------------------------
# Database pools
#
# OpenACS uses three pools by default:
#   pool1 -- main pool, most queries
#   pool2 -- second pool (e.g., nested queries, not generally recommended)
#   pool3 -- optional third pool, used by some packages/tools
#
# Make sure to set the db_* variables (db_host, db_port, db_name, db_user,
# db_password, db_pool, etc.) at the top of the file.
#
# In general, NaviServer can have different pools connecting to different databases
# and different database servers.  See:
#
#     http://openacs.org/doc/tutorial-second-database
#
#---------------------------------------------------------------------
ns_section ns/db/pools {
    ns_param pool1 "Pool 1"
    ns_param pool2 "Pool 2"
    ns_param pool3 "Pool 3"
}

# Pool 1 -- main pool
ns_section ns/db/pool/pool1 {
    # ns_param maxidle       0     ;# time until idle connections are closed; default: 5m
    # ns_param maxopen       0     ;# max lifetime of connections; default: 60m
    # ns_param checkinterval 5m    ;# check interval for stale handles

    ns_param connections     15
    ns_param LogMinDuration  10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param logsqlerrors    $debug
    ns_param datasource      $datasource
    ns_param user            $db_user
    ns_param password        $db_password
    ns_param driver          $db_driver_name
}

# Pool 2 -- secondary pool (e.g., nested queries)
ns_section ns/db/pool/pool2 {
    # ns_param maxidle       0
    # ns_param maxopen       0
    # ns_param checkinterval 5m    ;# check interval for stale handles

    # ns_param connections   2     ;# default: 2
    ns_param LogMinDuration  10ms  ;# when SQL logging is on, log only statements above this duration
    ns_param logsqlerrors    $debug
    ns_param datasource      $datasource
    ns_param user            $db_user
    ns_param password        $db_password
    ns_param driver          $db_driver_name
}

# Pool 3 -- optional third pool
ns_section ns/db/pool/pool3 {
    # ns_param maxidle       0
    # ns_param maxopen       0
    # ns_param checkinterval 5m    ;# check interval for stale handles

    # ns_param connections   2     ;# default: 2
    # ns_param LogMinDuration 0ms  ;# when SQL logging is on, log only statements above this duration
    ns_param logsqlerrors    $debug
    ns_param datasource      $datasource
    ns_param user            $db_user
    ns_param password        $db_password
    ns_param driver          $db_driver_name
}

#---------------------------------------------------------------------
# Experimental alternative DB driver -- extra module "nsdbipg"
#---------------------------------------------------------------------
if {"nsdbipg" in $extramodules} {
    ns_section ns/modules {
        ns_param nsdbipg1 nsdbipg.so
    }

    ns_section ns/module/nsdbipg1 {
        ns_param   default       true
        ns_param   maxhandles    40
        ns_param   timeout       10
        ns_param   maxidle       0
        ns_param   maxopen       0
        ns_param   maxqueries    0
        ns_param   maxrows       10000
        ns_param   datasource    "port=$db_port host=$db_host dbname=$db_name user=$db_user"
        ns_param   cachesize     [expr 1024*1024]
        ns_param   checkinterval 600
    }
}
