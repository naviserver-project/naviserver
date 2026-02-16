#---------------------------------------------------------------------
# WebSocket -- extra module "websocket"
#---------------------------------------------------------------------
# To use this module:
#   1. Install the NaviServer module websocket
#   2. Add websocket to servermodules
#   3. Configure websocket parameters as needed
#
if {"websocket" in $servermodules} {
    ns_section ns/server/$server/modules {
        ns_param websocket tcl
    }
    ns_section ns/server/$server/module/websocket/chat {
        ns_param urls     /websocket/chat
    }
    ns_section ns/server/$server/module/websocket/log-view {
        ns_param urls     /admin/websocket/log-view
        ns_param refresh  1000   ;# refresh time for file watcher in milliseconds
    }
}
