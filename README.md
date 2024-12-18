# Reverse Proxy Module for NaviServer

**Release 0.21**

*Author: neumann@wu-wien.ac.at*

The **revproxy** module provides a reverse proxy solution for NaviServer. It allows you to forward incoming requests to one or more backend systems. Both incoming and upstream connections can be handled via HTTP or HTTPS, and WebSocket (including secure WebSocket) connections are supported.

You can configure the module to selectively forward requests to backend servers based on HTTP method and URL patterns. This flexibility enables running a partially local, partially proxied site, where some requests are served locally by NaviServer and others are transparently handled by one or more backend services.

Additionally, you can invoke the proxy functionality (`revproxy::upstream`) directly from server-side pages (e.g., `.vuh` in OpenACS or ADP pages), allowing you to conditionally grant backend access only to authenticated or authorized users, such as administrators.

The revproxy functionality can be integrated into NaviServer’s request handling in three ways:

1. **As a Filter (`ns_register_filter`)**: Filters are applied early in the request lifecycle.
2. **As a Request Handler (`ns_register_proc`)**: Handlers run after authentication and permission checks.
3. **Direct Invocation in Server-Side Code**: Useful for fine-grained control over which requests are proxied.

Depending on your requirements—e.g., whether you need pre-authentication proxying or only after user authentication—you can choose the method that best fits your site’s security and performance model.

---

### Backend Connection Methods

The revproxy module supports two methods to connect to the backend:

1. **`ns_connchan` (Classical Method)**:
   - Event-driven, suitable for streaming HTML and WebSockets.
   - Time-tested and robust.

2. **`ns_http` (Experimental, Recommended for Performance)**:
   - Performs partial read/write at the C level for better efficiency.
   - Supports persistent connections (NaviServer 5.x+), improving performance for repeated connections to the same backend.
   - Integrates with writer threads, scaling well under heavy load.
   - Provides separate logs and statistics for backend connections.
   - For certain types of requests (e.g., the websocket upgrade
     request), we have to fall back to `ns_connchan`

You can choose the backend connection type by setting the `backendconnection` parameter.

---


## Configuration

To enable the reverse proxy functionality, load the module in your NaviServer configuration:

```tcl
ns_section "ns/server/${server}/modules" {
   ns_param revproxy tcl
}
```

You can use filters (pre-authentication), request handlers (post-authentication), or direct invocation. Filters run on all requests, including before authentication. Procedures registered via `ns_register_proc` run after authentication.

### Example: Using Request Filters (Post-Authentication)

Configure a `postauth` filter to remove the `/shiny` prefix before forwarding requests to `https://my.backend.com/`:

```tcl
ns_section "ns/server/${server}/module/revproxy" {
   ns_param filters {
     ns_register_filter preauth GET  /shiny/* ::revproxy::upstream -target https://my.backend.com/ -regsubs {{/shiny ""}}
     ns_register_filter preauth POST /shiny/* ::revproxy::upstream -target https://my.backend.com/ -regsubs {{/shiny ""}}
   }
}
```

### Example: Using a Request Handler

Forward `GET /doc/*` requests to two backend servers with a 20-second timeout:

```tcl
ns_section ns/server/default/module/revproxy {
   set target {http://server1.local:8080/ http://server2.local:8080/}
   ns_param filters [subst {
       ns_register_proc GET /doc {
           ::revproxy::upstream proc -timeout 20s -target [list ${target}]
       }
   }]
}
```

This setup supports a simple form of load distribution across multiple
backends based on a round robin principle. Currently, it does not
perform availability checks.

### Choosing the Backend Connection Method

```tcl
ns_section ns/server/default/module/revproxy {
   ns_param backendconnection ns_http  ;# default is ns_connchan
}
```

---

### Advanced Customization

The behavior of a proxy setup is defined by the parameters for the
proxy handler `revproxy::upstream`.
This command supports various parameters for fine-grained control:

- **Target URL:** The backend server(s) to which requests are forwarded.
- **Regsub Patterns (`-regsubs`):** Rewrite incoming URLs before forwarding.
- **URL Rewrite Callback (`-url_rewrite_callback`):** Dynamically construct the final upstream URL.
- **Exception Callback (`-exception_callback`):** Produce custom error pages on connection failures or other issues.
- **Validation Callback (`-validation_callback`):** Perform access checks before forwarding requests.
- **Backend Reply Callback (`-backend_reply_callback`):** Modify backend response headers before returning them to the client.
- **Timeout (`-timeout` defaults to `10s`):** Control how long to wait between events (connection, read or write)

These callbacks and parameters make it easy to tailor the proxying behavior to your application’s needs.

Note that many of such proxy handler can be defined with different
parameter configurations, some over filters, some over request
handlers, as the application requires.


Part of the advanced configuration is the setup of callbacks.
These callbacks can be freely defined by the application developer,
but the have to follow the signatures described below.

**URL Rewrite Callback (default: `::revproxy::rewrite_url`):**

This callback can be used to dynamically compute the target URL (including query parameters).

```tcl
#
# Definition of the default URL rewrite callback
#
nsf::proc ::revproxy::rewrite_url { -target -url {-query ""}} {
  set url [string trimright $target /]/[string trimleft $url /.]
  if {$query ne ""} {append url ?$query}
  return $url
}
```

**Exception Callback (default: `::revproxy::exception`):**

This callback can be used to handle errors and generate custom error pages.

```tcl
#
# Definition of the default exception callback
#
nsf::proc ::revproxy::exception {
  {-status 503}
  {-msg ""}
  -error
  -url
} {
  if {$msg eq ""} {
    ns_log warning "Opening connection to backend [ns_quotehtml $url] failed with status $status"
    set msg "Backend error: [ns_quotehtml $error]"
  }
  ns_returnerror $status $msg
}
```

**Validation Callback (no default):**

This callback can be used to perform access control based on the
details of the rewritten URL.  This is really the final check, after
also all the request headers for the backend have been set.

```tcl
#
# Define your own validation callback with a signature like this:
#
nsf::proc ::my_validation_callback {
  -url
} {
  # Implement custom logic, possibly returning a response or error if validation fails.
}
```

**Backend Reply Callback (no default):**

This callback can be used to modify headers returned by the backend
before sending the response to the client.

```tcl
#
# Define your own backend reply callback with a signature like this:
#
nsf::proc ::my_backend_replay_callback {
  -url
  -replyHeaders
  -status
} {
  # Modify replyHeaders as needed before sending to the client.
}
```

---

### Ensuring Network Drivers Are Loaded

Ensure that the appropriate network drivers (`nssock` for HTTP and
`nsssl` for HTTPS) are loaded. See the `ns_connchan` documentation for
more details.

If you do not want to listen on a particular port, but still need HTTPS capabilities for the backend, set the port to 0:

```tcl
ns_section ns/server/${server}/modules {
   ns_param revproxy tcl
   ns_param nsssl https
}

ns_section ns/server/${server}/module/https {
   ns_param port 0
}
```

This ensures `nsssl` is loaded without opening a listening port.

---

## Installation

```bash
make install
```

---

**Requirements:**

- NaviServer 4.99.21 or newer.

- `nsf` (Next Scripting Framework) from [http://next-scripting.org/](http://next-scripting.org/).

---


## Configuring NaviServer as a Reverse Proxy Server (Full Configuration File):

```tcl
########################################################################
# Sample configuration file for NaviServer with reverse proxy setup.
#
# Per default, the reverse proxy server uses the following configuration:
#
#    http                48080
#    https               48443
#    revproxy_target     http://127.0.0.1:8080
#    backend connection  ns_connchan
#
# These values can be overloaded via environment variables, when starting
# the server e.g. via
#
#    nsd_revproxy_target=https://localhost:8445 /usr/local/ns/bin/nsd -f ...
#
########################################################################
set http_port  [expr {[info exists env(nsd_httpport)]  ? $env(nsd_httpport)  : 48000}]
set https_port [expr {[info exists env(nsd_httpsport)] ? $env(nsd_httpsport) : 48443}]
set revproxy_target [expr {[info exists env(nsd_revproxy_target)] ? $env(nsd_revproxy_target) : "http://127.0.0.1:8080"}]
set revproxy_backendconnection [expr {[info exists env(nsd_revproxy_backendconnection)] ? $env(nsd_revproxy_backendconnection) : "ns_connchan"}]

set address "0.0.0.0"  ;# one might use as well for IPv6: set address ::
set home [file dirname [file dirname [info nameofexecutable]]]

########################################################################
# Global settings (for all servers)
########################################################################

ns_section ns/parameters {
    ns_param    home                $home
    ns_param    tcllibrary          tcl
    ns_param    serverlog           error.log
}

ns_section ns/servers {
    ns_param default "Reverse proxy"
}

#
# Global network modules (for all servers)
#
ns_section ns/modules {
    if {$https_port ne ""} { ns_param https nsssl }
    if {$http_port ne ""}  { ns_param http nssock }
}

ns_section ns/module/http {
    ns_param    port                     $http_port
    ns_param    address                  $address     ;# Space separated list of IP addresses
    #ns_param    maxinput                 500MB        ;# default: 1MB, maximum size for inputs (uploads)
    #ns_param    closewait                0s           ;# default: 2s; timeout for close on socket
    #
    # Spooling Threads
    #
    #ns_param   spoolerthreads		1	;# default: 0; number of upload spooler threads
    ns_param    maxupload		1MB     ;# default: 0, when specified, spool uploads larger than this value to a temp file
    ns_param    writerthreads		1	;# default: 0, number of writer threads
}

ns_section ns/module/https {
    ns_param    port                     $https_port
    ns_param    address                  $address     ;# Space separated list of IP addresses
    #ns_param    maxinput                 500MB        ;# default: 1MB, maximum size for inputs (uploads)
    #ns_param    closewait                0s           ;# default: 2s; timeout for close on socket
    #
    # ciphers, protocols and certificate
    #
    ns_param ciphers	"ECDH+AESGCM:DH+AESGCM:ECDH+AES256:DH+AES256:ECDH+AES128:DH+AES:ECDH+3DES:DH+3DES:RSA+AESGCM:RSA+AES:RSA+3DES:!aNULL:!MD5:!RC4"
    ns_param protocols	"!SSLv2:!SSLv3"
    ns_param certificate /usr/local/ns/etc/server.pem

    #
    # Spooling Threads
    #
    #ns_param   spoolerthreads          1       ;# default: 0; number of upload spooler threads
    ns_param    maxupload               1MB     ;# default: 0, when specified, spool uploads larger than this value to a temp file
    ns_param    writerthreads           1       ;# default: 0, number of writer threads
}

#
# Server mapping: define which DNS name maps to which NaviServer
# server configuration (here: server configuration "default").
#
ns_section ns/module/http/servers {
    ns_param default    localhost
    ns_param default    [ns_info hostname]
}

########################################################################
#  Settings for the "default" server
########################################################################

ns_section ns/server/default {
    #ns_param    enabletclpages      true  ;# default: false
    ns_param    connsperthread      1000  ;# default: 0; number of connections (requests) handled per thread
    ns_param    minthreads          5     ;# default: 1; minimal number of connection threads
    ns_param    maxthreads          100   ;# default: 10; maximal number of connection threads
    ns_param    maxconnections      100   ;# default: 100; number of allocated connection structures
    ns_param    rejectoverrun       true  ;# default: false; send 503 when thread pool queue overruns
}

ns_section ns/server/default/fastpath {
    #
    # From where to serve pages that are not redirected to a backend.
    #
    ns_param pagedir pages-revproxy
}

ns_section ns/server/default/modules {
    ns_param nslog nslog
    ns_param revproxy tcl
}


ns_section ns/server/default/module/revproxy {
    #
    # Configure the machinery for upstream connections. Options are
    #   - ns_connchan: features incremental spooling commands, low-level i/o commands
    #   - ns_http: features persistent connections to the backend server, provide statistics
    #
    ns_param backendconnection $revproxy_backendconnection  ;# default ns_connchan
    #ns_param verbose 1
    #ns_param streamingHtmlTimeout 2m

    #
    # Just for demonstration: use either request filter or request
    # handlers to register the callbacks.
    #
    set usefilter 0
    if {$usefilter} {
        ns_param register [subst {
            ns_register_filter postauth GET  /* ::revproxy::upstream -target [list $revproxy_target]
            ns_register_filter postauth POST /* ::revproxy::upstream -target [list $revproxy_target]
        }]
    } else {
        ns_param register [subst {
            ns_register_proc GET /* { ::revproxy::upstream proc -target [list ${revproxy_target}] }
            ns_register_proc POST /* { ::revproxy::upstream proc -target [list ${revproxy_target}] }
        }]
    }
}

set ::env(RANDFILE) $home/.rnd
set ::env(HOME) $home
set ::env(LANG) en_US.UTF-8
#
# For debugging, you might activate one of the following flags
#
#ns_logctl severity Debug(ns:driver) on
#ns_logctl severity Debug(request) on
#ns_logctl severity Debug(task) on
#ns_logctl severity Debug(sql) on
#ns_logctl severity Debug on
```

---

## Author

Gustaf Neumann <neumann@wu-wien.ac.at>
