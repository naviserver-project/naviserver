#---------------------------------------------------------------------
# NaviServer NaviServer Process Proxy -- core module "nsproxy"
# ---------------------------------------------------------------------
ns_section ns/server/$server/modules {
    ns_param nsproxy nsproxy
}
ns_section ns/server/$server/module/nsproxy {
    # ns_param	maxworker         8     ;# default: 8
    # ns_param	sendtimeout       5s    ;# default: 5s
    # ns_param	recvtimeout       5s    ;# default: 5s
    # ns_param	waittimeout       100ms ;# default: 1s
    # ns_param	idletimeout       5m    ;# default: 5m
    # ns_param	logminduration    1s    ;# default: 1s
}
