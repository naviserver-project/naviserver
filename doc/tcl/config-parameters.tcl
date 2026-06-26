namespace eval ::ns_configdoc {

    proc ::ns_configdoc::sectionSpec {sectionName} {
        variable data

        if {![dict exists $data sections]} {
            return ""
        }

        set bestSpec ""
        set bestScore -1

        dict for {sectionPattern sectionSpec} [dict get $data sections] {
            if {![string match $sectionPattern $sectionName]} {
                continue
            }

            set score [string length [string map {* ""} $sectionPattern]]

            if {$score > $bestScore} {
                set bestScore $score
                set bestSpec $sectionSpec
            }
        }

        return $bestSpec
    }

    proc ::ns_configdoc::SectionParamDoc {section parameter} {
        variable data

        if {![dict exists $data sections]} {
            return ""
        }

        set bestSpec ""
        set bestScore -1

        dict for {sectionPattern sectionSpec} [dict get $data sections] {
            if {![string match $sectionPattern $section]} {
                continue
            }

            #
            # Helper sections that provide reusable parameter documentation.
            #
            if {[dict exists $sectionSpec :providesParamDoc]} {
                set spec [ParamDocFromSet [dict get $sectionSpec :providesParamDoc] $parameter]
                if {$spec eq ""} {
                    continue
                }

            } else {
                if {[dict exists $sectionSpec $parameter]} {
                    set spec [dict get $sectionSpec $parameter]
                    set exactParameter 1
                } elseif {[dict exists $sectionSpec *]} {
                    set spec [dict get $sectionSpec *]
                    set exactParameter 0
                } else {
                    continue
                }
            }

            set score [string length [string map {* ""} $sectionPattern]]
            if {[info exists exactParameter] && $exactParameter} {
                incr score 10000
            }

            if {$score > $bestScore} {
                set bestScore $score
                set bestSpec $spec
            }

            unset -nocomplain exactParameter
        }

        return $bestSpec
    }

    proc ::ns_configdoc::ParamDocFromSet {name parameter} {
        variable data

        if {![dict exists $data paramdocs $name]} {
            return ""
        }

        set params [dict get $data paramdocs $name]

        if {[dict exists $params $parameter]} {
            return [dict get $params $parameter]
        }

        if {[dict exists $params *]} {
            return [dict get $params *]
        }

        return ""
    }

    proc ::ns_configdoc::ModuleParamDoc {implementation parameter} {
        variable data

        if {![dict exists $data modules $implementation]} {
            return ""
        }

        set moduleSpec [dict get $data modules $implementation]

        if {[dict exists $moduleSpec $parameter]} {
            return [dict get $moduleSpec $parameter]
        }

        if {[dict exists $moduleSpec *]} {
            return [dict get $moduleSpec *]
        }

        if {[dict exists $moduleSpec :includeParamDoc]} {
            foreach paramDoc [dict get $moduleSpec :includeParamDoc] {
                set spec [ParamDocFromSet $paramDoc $parameter]
                if {$spec ne ""} {
                    return $spec
                }
            }
        }

        if {[dict exists $moduleSpec :include]} {
            foreach parent [dict get $moduleSpec :include] {
                set spec [ModuleParamDoc $parent $parameter]
                if {$spec ne ""} {
                    return $spec
                }
            }
        }

        return ""
    }

    proc ::ns_configdoc::moduleImplementation {sectionName} {
        variable data

        #
        # Global module instance:
        #
        #   ns/module/http
        #
        # declared by:
        #
        #   ns/modules {
        #       ns_param http nssock
        #   }
        #
        if {[regexp {^ns/module/([^/]+)$} $sectionName . moduleName]} {
            set implementation [ns_config ns/modules $moduleName ""]
            if {$implementation ne ""} {
                return $implementation
            }

            #
            # Fallback for conventional instance names, e.g. ns/module/foo
            # where "foo" itself is documented as a module implementation.
            #
            if {[dict exists $data modules $moduleName]} {
                return $moduleName
            }

            return ""
        }

        #
        # Per-server module instance:
        #
        #   ns/server/default/module/nscp
        #
        # declared by:
        #
        #   ns/server/default/modules {
        #       ns_param nscp nscp
        #   }
        #
        if {[regexp {^ns/server/([^/]+)/module/([^/]+)$} $sectionName . server moduleName]} {
            set implementation [ns_config ns/server/$server/modules $moduleName ""]
            if {$implementation ne ""} {
                return $implementation
            }

            #
            # Fallback for conventional per-server module instance names, e.g.
            # ns/server/default/module/nscp. This keeps nsstats helpful even when
            # the section exists but the module registry entry is missing or not
            # loaded in the current configuration.
            #
            if {[dict exists $data modules $moduleName]} {
                return $moduleName
            }

            return ""
        }

        return ""
    }

    proc get {section parameter} {
        set parameter [string tolower $parameter]

        #
        # 1. Direct section-pattern lookup.
        #
        set spec [SectionParamDoc $section $parameter]
        if {$spec ne ""} {
            return $spec
        }

        #
        # 2. Module-instance lookup.
        #    The resolver is supplied by the application, e.g. nsstats,
        #    because it needs access to the live config database.
        #
        set implementation ""
        if {[namespace which ::ns_configdoc::moduleImplementation] ne ""} {
            set implementation [::ns_configdoc::moduleImplementation $section]
        }

        if {$implementation ne ""} {
            set spec [ModuleParamDoc $implementation $parameter]
            if {$spec ne ""} {
                return $spec
            }
        }

        return ""
    }

    proc tooltip {section parameter {default ""}} {

        set spec [::ns_configdoc::get $section $parameter]

        # ignorevalues, for which e cannot check defaults automatically.
        #  1st group: parameter just set/queried from tcl
        #  2nd group: computed values
        #  3rd group: sections with user-suppied key values
        set ignore {
            "ns/sendmail : 'smtpauthmode'"
            "ns/sendmail : 'smtpencoding'"
            "ns/sendmail : 'smtpencodingmode'"
            "ns/sendmail : 'smtphost'"
            "ns/sendmail : 'smtplogmode'"
            "ns/sendmail : 'smtpmsgid'"
            "ns/sendmail : 'smtpport'"
            "ns/sendmail : 'smtptimeout'"
            "ns/server/default : 'enablehttpproxy'"
            "ns/server/default : 'enabletclpages'"
            "ns/server/default/adp : 'enabletclpages'"
            "ns/server/default/fastpath : 'hidedotfiles'"
            "ns/server/default/module/nsperm : 'htaccess'"
            "ns/server/default/module/revproxy : 'verbose'"
            "ns/server/default/tcl : 'lazyloader'"
            "ns/server/oacs-head : 'enablehttpproxy'"
            "ns/server/oacs-head : 'enabletclpages'"
            "ns/server/oacs-head/acs : 'clustersecret'"
            "ns/server/oacs-head/acs : 'clustersecret'"
            "ns/server/oacs-head/acs : 'cookienamespace'"
            "ns/server/oacs-head/acs : 'parametersecret'"
            "ns/server/oacs-head/acs : 'staticcsp'"
            "ns/server/oacs-head/acs : 'whitelistedhosts'"
            "ns/server/oacs-head/adp : 'enabletclpages'"
            "ns/server/oacs-head/fastpath : 'hidedotfiles'"
            "ns/server/oacs-head/module/nsperm : 'htaccess'"
            "ns/server/oacs-head/module/revproxy : 'verbose'"
            "ns/server/oacs-head/redirects : '403'"
            "ns/server/oacs-head/redirects : '404'"
            "ns/server/oacs-head/redirects : '500'"
            "ns/server/oacs-head/redirects : '503'"
            "ns/server/oacs-head/tcl : 'lazyloader'"
            "ns/parameters : 'outputcharset'"
            "ns/module/nsstats : 'enabled'"
            "ns/module/nsstats : 'user'"

            "ns/mimetypes : 'noextension'"
            "ns/module/http : 'acceptsize'"
            "ns/module/http : 'backlog'"
            "ns/module/http : 'readahead'"
            "ns/module/http : 'uploadpath'"
            "ns/module/https : 'acceptsize'"
            "ns/module/https : 'backlog'"
            "ns/module/https : 'readahead'"
            "ns/module/https : 'uploadpath'"
            "ns/server/oacs-head/module/nsproxy : 'exec'"

            "ns/interps/cgiinterps : '.pl'"
            "ns/interps/cgiinterps : '.sh'"
            "ns/module/http/servers : 'default'"
            "ns/module/http/servers : 'oacs-head'"
            "ns/module/https/servers : 'default'"
            "ns/module/https/servers : 'oacs-head'"
            "ns/server/default/module/nscp/users : 'user'"
            "ns/server/default/modules : 'nscgi'"
            "ns/server/default/modules : 'nslog'"
            "ns/server/default/modules : 'nsperm'"
            "ns/server/default/modules : 'revproxy'"
            "ns/server/default: 'realm'"
            "ns/server/oacs-head : 'realm'"
            "ns/server/oacs-head/modules : 'libthread'"
            "ns/server/oacs-head/modules : 'nsdb'"
            "ns/server/oacs-head/modules : 'nslog'"
            "ns/server/oacs-head/modules : 'nsproxy'"

        }

        if {$spec eq ""} {
            if {"$section : '$parameter'" ni $ignore} {
                ns_log notice DEBUG $section : '$parameter' (default '$default'): no spec
            }
            return ""
        }

        if {"$section : '$parameter'" in $ignore} {
            # nothing to warn
        } elseif {$default eq "" && ![dict exists $spec default]} {
            #ns_log notice DEBUG $section : '$parameter' (default '$default'): no default in spec
        } elseif {$default eq "" && [dict exists $spec default] && [dict get $spec default] ne ""} {
            ns_log notice DEBUG $section : '$parameter' (default '$default'): wrong default in doc '[dict get $spec default]'
        } elseif {![dict exists $spec default]} {
            ns_log notice DEBUG $section : '$parameter' (default '$default'): no default
        } else {
            set docdefault [expr {![dict exists $spec default] ? "" : [dict get $spec default]}]
            if {![string equal $default $docdefault]} {
                ns_log notice DEBUG $section : '$parameter' (default '$default'): doc default '$docdefault' -\
                    > equal [string equal $default $docdefault]
            }
        }

        set result [dict get $spec desc]

        if {[dict exists $spec type] && [dict get $spec type] ne ""} {
            append result " (" [dict get $spec type] ")"
        }

        if {[dict exists $spec values] && [dict get $spec values] ne ""} {
            append result "\nPossible values: " [dict get $spec values]
        }
        if {[dict exists $spec deprecated] && [dict get $spec deprecated] ne ""} {
            append result "\nDeprecated: " [dict get $spec deprecated]
        }
        if {$default ne ""} {
            append result "\nDefault: " $default
        }

        return $result
    }
}


set ::ns_configdoc::data {

    paramdocs {
        network-driver {
            :title {Common Network Driver Parameters}
            :desc {
                These parameters are shared by HTTP and HTTPS network drivers. They
                control listen addresses and ports, request size limits, socket
                behavior, keep-alive handling, upload spooling, writer threads, and
                outgoing bandwidth limits.
            }

        }
    }

    sections {

        ns/driver/common {
            :providesParamDoc network-driver
            :desc {Shared network-driver defaults intended to be copied into concrete driver sections via ns_section -from}
        }

        ns/module/* {
            useModuleImplementation
        }

        ns/server/*/module/* {
            useModuleImplementation
        }

        ns/modules {
            :title {ns/modules}
            :desc {
                The ns/modules section defines global module instances. Each parameter
                name is a configuration-defined module instance name, and each value is
                the module implementation to load. For network drivers, the instance
                name is used as the final path component of the corresponding
                configuration section, such as ns/module/http.
            }
            :example {
                ns_section ns/modules {
                    ns_param http  nssock
                }

                ns_section ns/module/http {
                    ns_param port 8080
                }
            }

            * {
                key {module instance name}
                type module
                keySemantics identifier
                desc {Maps a global module instance name to a module implementation name}
            }
        }

        ns/servers {
            :title {ns/servers}
            :desc {
                The ns/servers section defines the virtual servers known to this
                NaviServer process. Each parameter name is a configuration-defined
                server name, and each value is a human-readable server description.
                The server name is used in per-server configuration sections such as
                ns/server/$server.
            }
            :example {
                ns_section ns/servers {
                    ns_param default "Default server"
                    ns_param intranet "Intranet server"
                }

                ns_section ns/server/default {
                    ns_param serverdir servers/default
                }
            }
            * {
                key {server name}
                type string
                keySemantics identifier
                desc {Maps a virtual server name to a human-readable server description}
            }
        }

        ns/fastpath {

            :title {ns/fastpath}
            :desc {
                The ns/fastpath section configures global behavior for static-file
                delivery. These settings control optional in-memory caching,
                memory-mapped file I/O, and support for pre-compressed gzip or Brotli
                variants of static files.

                Per-server file locations and directory handling are configured
                separately in the ns/server/$server/fastpath section.
            }

            :example {
                ns_section ns/fastpath {
                    # Global fastpath delivery settings.
                    ns_param cache false
                    ns_param mmap  false

                    # Serve existing .gz files when the client accepts gzip content.
                    ns_param gzip_static true

                    # Refresh stale .gz files by invoking ns_gzipfile.
                    ns_param gzip_refresh true
                    ns_param gzip_cmd     "/usr/bin/gzip -9"

                    # Serve existing .br files when the client accepts Brotli content.
                    ns_param brotli_static true

                    # Refresh stale .br files by invoking ns_brotlifile.
                    ns_param brotli_refresh true
                    ns_param brotli_cmd     "/usr/bin/brotli -f -q 11"

                    # Optional minification before gzip refresh for CSS and JavaScript.
                    # The commands must read from stdin and write to stdout.
                    # ns_param minify_css_cmd "/usr/bin/yui-compressor --type css"
                    # ns_param minify_js_cmd  "/usr/bin/yui-compressor --type js"
                }
            }

            brotli_cmd {
                type command
                desc {Use for re-compressing}
            }
            brotli_refresh {
                type boolean
                default false
                desc {Refresh stale .br files}
            }
            brotli_static {
                type boolean
                default false
                desc {Check for static brotli files}
            }
            cache {
                type boolean
                default false
                desc {Enable the global fastpath file-content cache for small static files; cached entries are validated against file mtime, size, device, and inode, while recently changed files and files larger than cachemaxentry are served directly}
            }
            cachemaxsize {
                type size
                default {10MB}
                desc {Maximum total memory used by the fastpath file-content cache; applies only when the fastpath cache parameter is enabled}
            }
            cachemaxentry {
                type size
                default {8KB}
                desc {Maximum size of an individual static file stored in the fastpath cache; files above this limit are not cached and are served directly from the filesystem or via mmap when enabled}
            }
            gzip_cmd {
                type command
                desc {Use for re-compressing}
            }
            gzip_refresh {
                type boolean
                default false
                desc {Refresh stale .gz files}
            }
            gzip_static {
                type boolean
                default false
                desc {Check for static gzip file}
            }
            mmap {
                type boolean
                default false
                desc {Use memory-mapped I/O for serving static files that are not cached; falls back to normal file reading when mmap is unavailable or disabled}

            }
            minify_css_cmd {
                type command
                default {}
                desc {Command used to minify CSS files when refreshing stale gzip-compressed static files; the command must read CSS from stdin and write minified CSS to stdout}
            }

            minify_js_cmd {
                type command
                default {}
                desc {Command used to minify JavaScript files when refreshing stale gzip-compressed static files; the command must read JavaScript from stdin and write minified JavaScript to stdout}
            }
        }

        ns/mimetypes {
            :desc {
                The ns/mimetypes section defines MIME type mappings used when
                NaviServer determines the content type of a file from its filename.
                These mappings are used when serving static files and by APIs such as
                ns_guesstype.

                NaviServer includes a large built-in MIME type table based on IANA
                media type registrations. Since lookup is based on filename
                extensions, real-world usage can still require local overrides when
                the same extension is used for different content types or when local
                conventions differ from the built-in defaults.
            }
            :example {
                ns_section ns/mimetypes {
                    ns_param default     text/plain
                    ns_param noextension text/plain

                    # Override or add extension-specific mappings.
                    ns_param .md         text/markdown
                    ns_param .log        text/plain
                    ns_param .wasm       application/wasm
                }
            }
            default {
                type mime-type
                default {*/*}
                desc {MIME type used when no more specific mapping is found for a requested file}
            }

            noextension {
                type mime-type
                default {*/*}
                desc {MIME type used for requested files whose names do not have a filename extension; when unset, the value of default is used}
            }

            * {
                key {filename extension}
                keySemantics mapping
                type mime-type
                cardinality many
                desc {Maps a filename extension to a MIME content type, overriding or extending the built-in MIME type table}
            }
        }

        ns/parameters {
            :title0 {Global Parameters}
            :desc {
                The ns/parameters section contains global NaviServer settings that are
                read during startup or used by process-wide subsystems. It includes the
                installation directory layout, global logging behavior, DNS caching,
                scheduler and job defaults, startup behavior, and compatibility settings.
            }

            asynclogwriter {
                type boolean
                default false
                desc {Write error.log and access.log asynchronously using writer threads.}
            }
            autosni {
                type boolean
                default true
                desc {Enable SNI}
            }

            bindir {
                type path
                default {bin}
                desc {Name of the directory used for loading binary libraries or executables such as nsproxy workers; relative paths are resolved against the home directory}
            }
            cachingmode {
                type enum
                values {full none}
                default {full}
                desc {Controls ns_cache behavior; full enables normal caching, while none makes ns_cache operations no-ops for conservative cluster deployments}
            }
            concurrentinterpcreate {
                type boolean
                default true
                desc {Allow Tcl interpreters to be created concurrently; when disabled, interpreter creation is serialized}
            }
            dnscache {
                type boolean
                default true
                desc {Enable DNS result caching}
            }
            dnscachemaxsize {
                type size
                default {500KB}
                desc {Max in-memory size of DNS cache}
            }
            dnscachetimeout {
                type time
                default {60m}
                desc {Time to keep entries in cache}
            }
            dnswaittimeout {
                type time
                default {5s}
                desc {Timeout for DNS replies}
            }
            home {
                type path
                desc {Root directory of the NaviServer installation; used as the base directory for relative paths in ns/parameters}
            }
            joblogminduration {
                type time
                default {1s}
                desc {Log only jobs longer than this}
            }
            jobsperthread {
                type integer
                default 0
                desc {Number of ns_jobs processed by a worker thread before it exits}
            }
            jobtimeout {
                type time
                default 5m
                desc {Default timeout for ns_job.}
            }
            listenbacklog {
                type integer
                default 32
                desc {Backlog for ns_socket}
            }
            logcolorize {
                type boolean
                default false
                desc {ANSI-colored log output}
            }
            logdebug {
                type boolean
                default false
                desc {Debug messages}
            }
            logexpanded {
                type boolean
                default false
                desc {Use expanded log formatting by placing the message on an indented separate line and adding a blank line after each entry; useful for manual debugging}
            }
            logdir {
                type path
                default {logs}
                desc {Name of the directory used for log files and early startup files such as the pid file; relative paths are resolved against the home directory}
            }

            logdeduplicate {
                type boolean
                default false
                desc {Collapse repeated identical log messages}
            }
            logdev {
                type boolean
                default false
                desc {Development messages}
            }
            logmaxbackup {
                type integer
                default 10
                desc {Number of rotated logs to keep}
            }
            lognotice {
                type boolean
                default true
                desc {Informational messages}
            }
            logprefixcolor {
                type enum
                default green
                values {black red green yellow blue magenta cyan gray default}
                desc {Color used for log prefixes}
            }
            logprefixintensity {
                type enum
                default normal
                values {bright normal}
                desc {Intensity used for log prefixes}
            }
            logrelative {
                type boolean
                default false
                desc {Start timestamps from zero}
            }
            logrollfmt {
                type string
                desc {Suffix appended to logfile name when rolled}
            }
            logrollonsignal {
                type boolean
                default true
                desc {Rotate log on SIGHUP}
            }
            logsec {
                type boolean
                default true
                desc {Timestamps in seconds}
            }
            logthread {
                type boolean
                default true
                desc {Include thread id in log lines}
            }
            logusec {
                type boolean
                default false
                desc {Timestamps in microseconds}
            }
            logusecdiff {
                type boolean
                default false
                desc {Deltas between log entries in microseconds}
            }
            maxconcurrentupdates {
                type integer
                default {1000}
                desc {Maximum number of Tcl interpreters that may concurrently run update scripts after the server Tcl epoch changes}
            }
            mutexlocktrace {
                type boolean
                default false
                desc {Enable tracing of mutex lock operations for debugging locking behavior}
            }
            nshttptaskthreads {
                type integer
                default 1
                desc {Number of task threads}
            }
            pidfile {
                type path
                default {nsd.pid}
                desc {Path of the file storing the nsd process id; relative paths are resolved against the log directory}
            }
            progressminsize {
                type size
                default 0
                desc {Minimum upload size for enabling upload progress tracking; useful for clients that display progress bars for large uploads, while avoiding progress bookkeeping overhead for small requests}
            }
            rejectalreadyclosedconn {
                type boolean
                default true
                desc {Reject and log attempts by application code to send output on an already closed or detached connection, such as a second ns_return for the same request; prevents undefined behavior and confusing partial output, while false preserves legacy behavior}
            }
            reverseproxymode {
                type boolean
                default false
                deprecated {Use the ns/reverseproxymode section instead}
                desc {Legacy switch for enabling reverse-proxy mode; deprecated in favor of the ns/reverseproxymode section, which provides explicit control over trusted proxies and filtering of forwarded client addresses}
            }
            sanitizelogfiles {
                type integer
                default {2}
                desc {Sanitize log file names; 0 = none, 1 = full, 2 = human-friendly}
            }
            schedlogminduration {
                type time
                default {2s}
                desc {Log scheduled jobs that run longer than this}
            }
            schedsperthread {
                type integer
                default 0
                desc {Number of scheduled jobs processed by a scheduler thread before it exits}
            }
            shutdowntimeout {
                type time
                default {20s}
                desc {Maximum time to wait during shutdown for threads and subsystems to terminate cleanly}
            }
            sockacceptlog {
                type integer
                default {4}
                desc {Log repeated accepts after this threshold}
            }
            stacksize {
                type size
                deprecated {Use ns/threads stacksize instead}
                desc {Legacy location for the default thread stack size; when unset or 0, the operating system default is used}
            }
            systemlog {
                type path
                default {nsd.log}
                desc {Path of the main server log file; relative paths are resolved against the log directory}
            }

            tcllibrary {
                type path
                default {tcl}
                desc {Directory containing NaviServer Tcl library files; relative paths are resolved against the home directory}
            }
            tclinitlock {
                type boolean
                default false
                desc {Serialize Tcl interpreter initialization; usually disabled, but useful when debugging rare initialization crashes or when running under tools such as valgrind}
            }
            tmpdir {
                type path
                desc {Directory for temporary files; when unset, NaviServer uses the TMPDIR environment variable or the system default temporary directory, with a trailing slash removed}
            }

            outputcharset {
                type charset
                default {utf-8}
                desc {Global default for the per-server outputcharset setting; used when ns/server/$server outputcharset is not configured}
            }

            urlcharset {
                type charset
                default {utf-8}
                desc {Global default for the per-server urlcharset setting; used when ns/server/$server urlcharset is not configured}
            }

            formfallbackcharset {
                type charset
                desc {Global default for the per-server formfallbackcharset setting; used when ns/server/$server formfallbackcharset is not configured}
            }
            
        }

        ns/reverseproxymode {
            :title {ns/reverseproxymode}
            :desc {
                The ns/reverseproxymode section configures how NaviServer interprets
                request information when it runs behind trusted reverse proxies or load
                balancers. These settings affect the effective client peer address used
                by access logs and APIs that report peer-address information.

                This section is about NaviServer being behind a reverse proxy. It is
                unrelated to the revproxy module, where NaviServer itself forwards
                requests to backend servers.
            }

            enabled {
                type boolean
                default false
                desc {Enable reverse-proxy mode for installations behind trusted frontend proxies or load balancers; affects the effective peer address used by access logs and APIs that determine the client peer address}
            }

            skipnonpublic {
                type boolean
                default false
                desc {Ignore forwarded client addresses that are not public addresses; useful to avoid recording private or internal proxy-chain addresses as the effective client peer}
            }

            trustedservers {
                type list
                desc {Trusted proxy addresses or networks whose forwarded client information may be accepted; only proxies listed here should be allowed to influence the effective peer address used by access logs and peer-address APIs}
            }
        }

        ns/parameters/reverseproxymode {
            :title {ns/parameters/reverseproxymode}
            :deprecated {Use ns/reverseproxymode instead}
            :desc {
                Deprecated compatibility section for reverse-proxy mode configuration.
                New configurations should use ns/reverseproxymode.
            }

            enabled {
                type boolean
                default false
                deprecated {Use ns/reverseproxymode enabled instead}
                desc {Compatibility setting for enabling reverse-proxy mode}
            }

            skipnonpublic {
                type boolean
                default false
                deprecated {Use ns/reverseproxymode skipnonpublic instead}
                desc {Compatibility setting for ignoring non-public forwarded client addresses}
            }

            trustedservers {
                type list
                deprecated {Use ns/reverseproxymode trustedservers instead}
                desc {Compatibility setting for trusted reverse proxy addresses or networks}
            }
        }

        ns/sendmail {
            :title {ns/sendmail}
            :desc {
                The ns/sendmail section documents configuration parameters used by the
                legacy ns_sendmail command. The command sends mail through a configured
                SMTP server and provides only basic SMTP support. STARTTLS is available
                only when the external Tcl package "tls" is installed and loadable.

                For new applications, prefer the external nssmtpd module where appropriate,
                or Tcl library based mail support with modern TLS-capable SMTP handling.

                In earlier NaviServer versions, these parameters were configured directly
                in ns/parameters. This legacy location is still accepted for compatibility,
                but new configurations should use ns/sendmail.
            }

            :see {
                {uri https://github.com/naviserver-project/nssmtpd nssmtpd}
            }

            smtphost {
                type hostname
                default {localhost}
                desc {SMTP server host used by ns_sendmail}
            }

            smtpport {
                type integer
                default {25}
                desc {SMTP server port used by ns_sendmail}
            }

            smtptimeout {
                type time
                default {60}
                desc {Timeout for SMTP operations performed by ns_sendmail}
            }

            smtplogmode {
                type boolean
                default false
                desc {Enable logging of SMTP dialogue or delivery activity for ns_sendmail}
            }

            smtpmsgid {
                type boolean
                default false
                desc {Generate a Message-ID header for messages sent by ns_sendmail}
            }

            smtpmsgidhostname {
                type hostname
                default {}
                desc {Host name used when generating Message-ID headers for ns_sendmail; when empty, the default host name is used}
            }

            smtpencodingmode {
                type boolean
                default false
                desc {Enable message encoding support for ns_sendmail}
            }

            smtpencoding {
                type charset
                default {utf-8}
                desc {Character encoding used by ns_sendmail when SMTP encoding mode is enabled}
            }

            smtpauthmode {
                type enum
                values {PLAIN LOGIN}
                default {PLAIN}
                desc {SMTP authentication mode used by ns_sendmail; empty value disables SMTP authentication}
            }

            smtpauthuser {
                type string
                default {}
                desc {SMTP authentication user name used by ns_sendmail}
            }

            smtpauthpassword {
                type password
                default {}
                desc {SMTP authentication password used by ns_sendmail}
            }
        }

        ns/server/* {
            :title {ns/server/$server}
            :desc {
                The ns/server/$server section defines core settings for a virtual
                server.  It controls connection-thread behavior, compression defaults,
                request-overload handling, system-generated notices, default character
                sets, and selected compatibility options.

                The placeholder $server is the server name defined in ns/servers.
            }

            checkmodifiedsince {
                type boolean
                default true
                desc {Honour If-Modified-Since for cached files}
            }
            compressenable {
                type boolean
                default false
                desc {Enable gzip compression for eligible dynamic responses by default; individual requests can still control compression via ns_conn compress}
            }
            compresslevel {
                type {integer 1-9}
                default {4}
                desc {Compression level; higher values use more CPU and provide better compression}
            }
            compressminsize {
                type size
                default {512}
                desc {Compress responses larger than this size}
            }
            compresspreinit {
                type boolean
                default false
                desc {Preallocate compression buffers at startup}
            }
            connectionratelimit {
                type size
                default 0
                desc {Rate limit per connection; -1 means unlimited}
            }
            connsperthread {
                type integer
                default {10000}
                desc {Number of requests processed by a connection thread before it exits; 0 means unlimited}
            }
            enablehttpproxy {
                type boolean
                default false
                desc {Enable forward HTTP proxy handling for this server; for scalable proxying, the revproxy module should normally be loaded, otherwise a simple fallback implementation is used}
            }
            enabletclpages {
                type boolean
                default false
                desc {Enable direct execution of .tcl pages for GET, HEAD, and POST requests by registering /*.tcl as Tcl request handlers}
            }

            errorminsize {
                type size
                default {514}
                desc {Pad error replies up to this size}
            }
            extraheaders {
                type ns_set
                desc {Additional HTTP response headers added for this server, typically used for server-wide security headers or policy headers}
            }
            filterrwlocks {
                type boolean
                default true
                desc {Use read/write locks for managing request filters; this improves concurrency when filters are mostly read during request processing and only changed occasionally}
            }
            highwatermark {
                type integer
                default {80}
                desc {Allow concurrent thread creation above this queue fill percentage}
            }
            logdir {
                type path
                desc {Directory for per-server log files; relative paths are resolved against the server home directory}
            }
            lowwatermark {
                type integer
                default {10}
                desc {Create more threads above this queue fill percentage}
            }
            maxconnections {
                type integer
                default {100}
                desc {Maximum number of connection structures allocated for this connection pool.
                    These structures cover both requests currently assigned to connection threads and
                    requests waiting in the pool queue. When all structures are in use, the pool has
                    reached its connection limit. If rejectoverrun is enabled, additional requests
                    for this pool are rejected. Otherwise, the socket remains managed by the
                    driver and may be retried according to the driver's queue handling.
                }
            }
            maxthreads {
                type integer
                default {10}
                desc {Maximum number of connection threads}
            }
            minthreads {
                type integer
                default 1
                desc {Minimum number of connection threads}
            }

            noticeadp {
                type path
                default {returnnotice.adp}
                desc {ADP template used for system-generated HTTP notice responses such as ns_returnnotice; relative paths are resolved against SERVERHOME/conf, an empty value uses a built-in template, and errors in a custom template fall back to a basic built-in template}
            }

            noticedetail {
                type boolean
                default true
                desc {Include server signature}
            }

            poolratelimit {
                type size
                default 0
                desc {Rate limit per pool; 0 means unlimited}
            }

            realm {
                type string
                desc {Default HTTP Basic authentication realm used in WWW-Authenticate responses when no connection-specific realm is set; defaults to $server}
            }

            rejectoverrun {
                type boolean
                default false
                desc {
                    Reject requests with 503 Service Unavailable when
                    the selected connection pool has reached its
                    maxconnections limit and no connection structure
                    is available. When enabled, driver-level queueing
                    and later retry for this pool-overrun condition
                    are disabled, so overload is reported immediately
                    to the client. When false, the socket remains
                    managed by the network driver and may be retried
                    later for queueing into the selected pool, subject
                    to the driver's maxqueuesize. Enabling this option
                    applies back-pressure during overload.
                }
            }

            retryafter {
                type time
                default {5s}
                desc {Number of seconds used for the Retry-After header on pool-overrun 503 responses (i.e., rejectoverrun is active); 0 disables the header.}
            }
            serverdir {
                type path
                default {}
                desc {Root directory for this virtual server; relative paths are resolved against the NaviServer home directory. This setting is the canonical location for the per-server directory and replaces the deprecated serverdir setting in ns/server/$server/fastpath}
            }
            serverrootproc {
                type proc
                desc {Tcl procedure used to compute the server root directory dynamically, for example for mass virtual hosting based on the request host}
            }
            stealthmode {
                type boolean
                default false
                desc {Omit Server header}
            }

            threadtimeout {
                type time
                default {2m}
                desc {Idle timeout for connection threads above minthreads; excess threads exit after being idle for this duration}
            }
            outputcharset {
                type charset
                default {utf-8}
                desc {Default character set for text output from this server; used to determine the Tcl encoding for converting Tcl UTF-8 strings to the response encoding; defaults to value of outputcharset from ns/parameters}
            }            
            urlcharset {
                type charset
                default {utf-8}
                desc {Default character set for decoding URL query strings and form data for this server; defaults to value of urlcharset from ns/parameters}
            }
            formfallbackcharset {
                type charset
                desc {Fallback character set used when parsing form data if no explicit charset can be determined from the request; defaults to value of formfallbackcharset from ns/parameters}
            }
            
        }

        ns/server/*/pools {
            :title {ns/server/$server/pools}
            :desc {
                The ns/server/$server/pools section defines named connection thread
                pools for a virtual server. Each parameter name is a
                configuration-defined pool name, and each value is a human-readable
                description of the pool.

                Connection pools can be used to route selected requests to dedicated
                worker threads, for example for monitoring URLs, fast static requests,
                slow requests, or bot traffic. Requests not matching any per-pool map
                entry are handled by the server's default connection pool.
            }

            :example {
                ns_section ns/server/$server/pools {
                    ns_param monitor "Monitoring and health-check requests"
                    ns_param fast    "Fast static-file requests"
                    ns_param slow    "Slow request pool"
                    ns_param bots    "Crawler and bot traffic"
                }
            }

            * {
                key {pool name}
                keySemantics identifier
                type string
                cardinality many
                desc {Configuration-defined connection pool name whose value is a human-readable pool description; the pool name is used in ns/server/$server/pool/$pool sections}
            }
        }

        ns/server/*/pool/* {
            :title {ns/server/$server/pool/$pool}
            :desc {
                The ns/server/$server/pool/$pool section configures a named connection
                thread pool. The parameters in this section correspond to selected
                parameters from ns/server/$server and override the per-server values for
                requests assigned to this pool.

                Requests are assigned to a named pool by map entries. Each map entry
                matches an HTTP method and URL pattern, optionally followed by context
                constraints such as user-agent patterns. Requests that do not match any
                configured pool map are handled by the server's default connection pool.
            }

            :example {
                ns_section ns/server/$server/pool/monitor {
                    ns_param minthreads 2
                    ns_param maxthreads 2

                    ns_param map "GET  /SYSTEM"
                    ns_param map "POST /SYSTEM"
                    ns_param map "GET  /admin/nsstats"
                    ns_param map "GET  /admin/nsstats.tcl"
                }

                ns_section ns/server/$server/pool/bots {
                    ns_param map "GET /* {user-agent *bot*}"
                    ns_param map "GET /* {user-agent *crawl*}"

                    ns_param minthreads     2
                    ns_param maxthreads     2
                    ns_param rejectoverrun  true
                    ns_param poolratelimit  1000
                }
            }

            map {
                type mapping
                cardinality multimap
                desc {Pool mapping entry; each occurrence maps an HTTP method and URL pattern to this connection pool, optionally followed by context constraints such as user-agent patterns}
            }

            connsperthread {
                type integer
                desc {Number of requests processed by a connection thread in this pool before it exits}
            }

            highwatermark {
                type integer
                desc {Queue fill percentage above which connection threads for this pool may be created in parallel}
            }

            lowwatermark {
                type integer
                desc {Queue fill percentage above which an additional connection thread for this pool may be created}
            }

            maxconnections {
                type integer
                desc {Maximum number of allocated connection structures for this pool}
            }

            minthreads {
                type integer
                desc {Minimum number of connection threads kept for this pool}
            }

            maxthreads {
                type integer
                desc {Maximum number of connection threads for this pool}
            }

            rejectoverrun {
                type boolean
                desc {Reject requests assigned to this pool with 503 Service Unavailable when the pool queue is full}
            }

            retryafter {
                type time
                desc {Value used for the Retry-After header in 503 Service Unavailable responses from this pool when rejectoverrun is enabled}
            }

            threadtimeout {
                type time
                desc {Idle timeout for connection threads in this pool above minthreads}
            }

            poolratelimit {
                type integer
                desc {Default outgoing bandwidth limit in kilobytes per second for each connection in this pool; 0 means unlimited and per-connection limits take precedence}
            }

            connectionratelimit {
                type integer
                desc {Outgoing bandwidth limit in kilobytes per second for individual connections in this pool; 0 means unlimited}
            }
        }


        ns/server/*/adp {
            :title {ns/server/$server/adp}
            :desc {
                The ns/server/$server/adp section configures ADP processing for a
                virtual server, including ADP caching, buffering, error handling,
                debugging, streaming, and URL mappings for ADP pages.
            }
            :example {
                ns_section ns/server/$server/adp {
                    # Register ADP pages.
                    ns_param map         /*.adp

                    # Development/debugging.
                    ns_param enabledebug $debug

                    # Optional cache and buffer tuning.
                    # ns_param cache     true
                    # ns_param cachesize 10MB
                    # ns_param bufsize   5MB

                    # Optional common processing and error handling hooks.
                    # ns_param startpage /path/to/startpage.adp
                    # ns_param errorpage /path/to/errorpage.adp
                }
            }

            autoabort {
                type boolean
                default true
                desc {Failure to flush a buffer generates an ADP exception}
            }
            bufsize {
                type size
                default {1MB}
                desc {Size of ADP buffer}
            }
            cache {
                type boolean
                default false
                desc {Enable ADP caching}
            }
            cachesize {
                type size
                default {5MB}
                desc {Size of ADP cache}
            }
            debuginit {
                type proc
                default ns_adp_debuginit
                desc {Procedure to execute on ADP debug initialization}
            }
            defaultextension {
                type string
                default {}
                desc {Optional file extension appended to unresolved ADP page filenames; useful when requests omit the ADP extension, for example resolving a request for page to page.adp}
            }
            detailerror {
                type boolean
                default true
                desc {Include connection info in error backtrace}
            }
            displayerror {
                type boolean
                default false
                desc {Include error message in output}
            }
            enabledebug {
                type boolean
                default false
                desc {URL pattern registered for ADP processing; each map entry registers the ADP handler for GET, HEAD, and POST requests on the specified pattern}
            }
            enableexpire {
                type boolean
                default false
                desc {Set Expires: now on all ADPs}
            }
            errorpage {
                type path
                default {}
                desc {Page for returning errors}
            }
            map {
                type list
                desc {URL pattern registered for ADP processing; each entry maps the pattern for GET, HEAD, and POST requests}
            }
            safeeval {
                type boolean
                default false
                desc {Disable inline scripts}
            }
            singlescript {
                type boolean
                default false
                desc {Collapse Tcl blocks to a single Tcl script; in singlescript mode, an error in any part of the combined script
stops execution of that ADP page}
            }
            startpage {
                type path
                default {}
                desc {File to run for every ADP request}
            }
            stream {
                type boolean
                default false
                desc {Enable ADP streaming}
            }
            stricterror {
                type boolean
                default false
                desc {Interrupt execution on any error}
            }
            trace {
                type boolean
                default false
                desc {Trace execution of ADP scripts}
            }
            tracesize {
                type integer
                default {40}
                desc {Maximum number of entries in ADP trace}
            }
            trimspace {
                type boolean
                default false
                desc {Trim whitespace from output buffer}
            }
        }

        ns/server/*/fastpath {
            :title {ns/server/$server/fastpath}
            :desc {
                The ns/fastpath section configures global behavior for fast static-file
                delivery. It controls optional in-memory caching, memory-mapped file I/O,
                and support for pre-compressed gzip or Brotli variants of static files.
                Per-server fastpath settings, such as the page directory and directory
                listing behavior, are configured separately under ns/server/*/fastpath.
            }

            directoryfile {
                type list
                default {index.adp index.tcl index.html index.htm}
                desc {List of filenames to try when a request maps to a directory; the first existing file is served as the directory index}
            }
            directorylisting {
                type enum
                values {simple fancy none}
                desc {Directory listing style}
            }
            directoryadp {
                type path
                default {}
                desc {ADP page invoked to generate a directory listing when a request maps to a directory and no directory index file is found; when unset, NaviServer falls back to directoryproc if configured}
            }
            directoryproc {
                type proc
                default {_ns_dirlist}
                desc {Tcl procedure used to generate directory listings when no directory index file is found and directory listing is enabled}
            }
            pagedir {
                type path
                default {pages}
                desc {Root directory for static files and fastpath content for this server; relative paths are resolved against the server home directory}
            }

            hidedotfiles {
                type boolean
                default true
                desc {Hide files and directories whose names start with a dot from generated directory listings}
            }
        }


        ns/server/*/acs/acs-api-browser {
            includecallinginfo {
                type boolean
                desc {Useful mostly on development instances}
            }
        }

        ns/server/*/acs/acs-mail-lite {
            emaildeliverymode {
                type enum
                values {default log redirect nssmtpd}
                desc {Email delivery mode}
            }
        }


        ns/server/*/httpclient {
            :title {ns/server/$server/httpclient}
            :desc {
                The ns/server/$server/httpclient section defines per-server defaults
                for outgoing HTTP and HTTPS requests made through ns_http and
                ns_connchan.  It controls default timeouts, connection reuse, logging,
                and TLS certificate validation.
            }
            :see {
                {command ns_http}
                {command ns_connchan}
            }

            cafile {
                type path
                default {ca-bundle.crt}
                desc {CA bundle file containing trusted top-level certificates for outgoing HTTPS requests; relative paths are resolved against the home directory}
            }

            capath {
                type path
                default {certificates}
                desc {Directory containing trusted CA certificates for outgoing HTTPS requests; relative paths are resolved against the home directory}
            }

            defaulttimeout {
                type time
                default {5s}
                desc {Default timeout for outgoing ns_http requests when neither -timeout nor -expire is specified}
            }

            invalidcertificates {
                type path
                default {invalid-certificates}
                desc {Directory where invalid peer certificates from outgoing HTTPS requests are stored for inspection; relative paths are resolved against the home directory}
            }
            keepalive {
                type time
                default {0s}
                desc {Default keep-alive timeout for outgoing ns_http requests; 0 disables connection reuse by default}
            }

            logging {
                type boolean
                default false
                desc {Enable logging for outgoing HTTP client requests}
            }
            logmaxbackup {
                type integer
                default 100
                desc {Maximum number of backup log files}
            }
            logroll {
                type boolean
                default true
                desc {Rotate log files automatically}
            }
            logrollfmt {
                type string
                default {}
                desc {Format appended to log filename}
            }
            logrollhour {
                type integer
                default 0
                desc {Hour at which to roll the log}
            }
            logrollonsignal {
                type boolean
                default false
                desc {Perform log rotation on SIGHUP}
            }
            validatecertificates {
                type boolean
                default true
                desc {Validate TLS certificates for outgoing ns_http and ns_connchan HTTPS requests; disabling validation is insecure and increases exposure to man-in-the-middle attacks}
            }

            validationdepth {
                type integer
                default {9}
                desc {Maximum allowed certificate chain validation depth for outgoing HTTPS requests; 0 accepts only self-signed certificates, 1 accepts self-signed or certificates issued by one CA, and higher values allow longer chains}
            }

            validationexception {
                type list
                desc {Whitelist certificate validation exceptions for outgoing HTTPS requests, optionally restricted by client IP or network; useful for controlled exceptions while still collecting invalid certificates for review}
            }
        }

        ns/server/*/tcl {
            :title {ns/server/$server/tcl}
            :desc {
                The ns/server/$server/tcl section configures Tcl interpreter
                initialization and server-specific Tcl behavior, including the init
                file, Tcl library location, lazy loading, memoization, and NSV locking
                settings.
            }

            initfile {
                type path
                default {bin/init.tcl}
                desc {Tcl initialization file sourced when initializing interpreters for this server; relative paths are resolved against the home directory}
            }

            errorlogheaders {
                type list
                desc {List of request header field names to include in Tcl error log messages for connection-related errors; matching headers are appended to the logged request method, URL, and peer address}
            }

            lazyloader {
                type boolean
                default false
                desc {Enable lazy loading of Tcl library procedures, loading definitions on demand instead of during interpreter initialization; note: not supportend for e.g. OpenACS}
            }

            library {
                type path
                default {modules/tcl}
                desc {Directory containing server-specific Tcl library files; relative paths are resolved against the home directory}
            }

            memoizecache {
                type size
                desc {Size of the Tcl memoization cache used by this server}
            }

            nsvbuckets {
                type integer
                default {8}
                desc {Number of hash buckets used for NSV storage; increasing the value can reduce lock contention for workloads with many shared variables}
            }

            nsvrwlocks {
                type boolean
                default true
                desc {Use read/write locks for NSV access; improves concurrency for read-heavy shared-variable workloads}
            }
        }

        ns/server/*/vhost {

            :title {ns/server/$server/vhost}
            :desc {
                The ns/server/$server/vhost section configures legacy host-based page
                root mapping within a single virtual server.  The request Host header
                is normalized and used to select a host-specific subdirectory below
                the server page root.

                This mechanism is less flexible than mass virtual hosting based on
                serverrootproc, but remains useful for simple legacy virtual-host
                layouts.
            }

            enabled {
                type boolean
                default false
                desc {
                    Enable legacy host-based page-root mapping within this server;
                    the request Host header is used to select a host-specific subdirectory
                    below the server page root. This mode requires a relative pagedir and is
                    less flexible than server-root based virtual hosting
                    via serverrootproc
                }
            }

            hostprefix {
                type path
                desc {Optional directory prefix inserted between the server directory and the
                    normalized host name when building the host-specific page root
                }
            }

            hosthashlevel {
                type integer
                default 0
                desc {
                    Number of hash-directory levels added before the normalized host name in
                    the generated page-root path; values from 0 to 5 are allowed and can avoid
                    too many host directories in one directory
                }
            }

            stripport {
                type boolean
                default true
                desc {Strip the port part from the Host header before deriving the host-specific page-root directory}
            }

            stripwww {
                type boolean
                default true
                desc {Strip a leading www. from the Host header before deriving the host-specific page-root directory}
            }
        }

        ns/server/*/module/revproxy/* {
            :title {revproxy backends}
            :desc {
                Backend sections below the revproxy module define individual upstream
                targets. The final path component is a configuration-defined backend
                name.

                Each backend specifies one or more target URLs and one or more map
                entries that select which incoming requests are forwarded to that
                backend. Optional settings control connection and transfer timeouts,
                URL rewriting, backend connection behavior, and context constraints.
            }

            :example {
                ns_section ns/server/$server/module/revproxy/backend1 {
                    ns_param target         https://backend.example.com/
                    ns_param connecttimeout 2s
                    ns_param receivetimeout 15s
                    ns_param sendtimeout    15s

                    ns_param map "GET  /api/*"
                    ns_param map "POST /api/* {-use_target_host_header true}"
                }
            }

            constraints {
                type context-constraints
                cardinality multimap
                desc {Context constraint entry; key/value pairs within one entry are combined with AND, while multiple constraints entries are combined with OR}
            }
            target {
                type {URI}
                desc {Upstream backend URL or list of backend URLs to which matching requests are forwarded}
            }

            map {
                type mapping
                cardinality multimap
                desc {Backend mapping entry; each occurrence maps an HTTP method and URL pattern to this backend, optionally followed by per-map options such as -use_target_host_header or -constraints}
            }
            connecttimeout {
                type time
                default {1s}
                desc {Maximum time to wait while establishing a connection to the backend server}
            }

            receivetimeout {
                type time
                default {10s}
                desc {Maximum time to wait for receiving data from the backend server}
            }

            sendtimeout {
                type time
                default {10s}
                desc {Maximum time to wait while sending request data to the backend server}
            }

            backendconnection {
                type enum
                values {s_http+ns_connchan ns_http ns_connchan}
                default ns_http+ns_connchan
                desc {Backend connection type for this backend; overrides the revproxy-wide backendconnection setting and can be used to select special forwarding behavior such as preauth}
            }

            regsubs {
                type list
                desc {Regular-expression substitutions applied to the forwarded URL before sending the request to the backend, for example to strip a URL prefix}
            }
        }

        ns/server/*/module/websocket/log-view {
            refresh {
                type integer
                desc {Refresh time for file watcher in milliseconds}
            }
        }

        ns/threads {
            :desc {
                The ns/threads section defines process-wide defaults for NaviServer
                threads. Currently, it is used to configure the default stack size for
                newly created threads.
            }
            stacksize {
                type size
                default 0kB
                desc {Default stack size for newly created threads; when unset or 0, the operating system default is used}
            }
        }

        ns/server/*/db {
            :title {ns/server/$server/db}
            :desc {
                The ns/server/$server/db section selects the database pools available
                to a virtual server. The pools themselves are defined globally under
                ns/db/pools and ns/db/pool/$dbpool and may be shared by multiple servers.

                The default pool is used when application code requests a database
                handle without explicitly naming a pool.
            }

            :example {
                ns_section ns/server/$server/db {
                    ns_param pools       pool1,pool2,pool3
                    ns_param defaultpool pool1
                }
            }

            pools {
                type list
                desc {List of database pool names available to this server; the names must refer to pools defined in ns/db/pools}
            }

            defaultpool {
                type string
                desc {Default database pool used by this server when no explicit pool name is requested}
            }
        }

        ns/db/pools {
            :title {ns/db/pools}
            :desc {
                The ns/db/pools section defines global database pool names. Each
                parameter name is a configuration-defined pool name, and each value is
                a human-readable description of the pool.

                Database pools are process-wide definitions and can be made available
                to one or more virtual servers through the server-specific
                ns/server/$server/db section.
            }

            :example {
                ns_section ns/db/pools {
                    ns_param pool1 "Main database pool"
                    ns_param pool2 "Secondary database pool"
                    ns_param pool3 "Optional third database pool"
                }
            }

            * {
                key {database pool name}
                keySemantics identifier
                type string
                cardinality many
                desc {Configuration-defined database pool name whose value is a human-readable pool description; the pool name is used in ns/db/pool/$dbpool sections and in per-server database configuration}
            }
        }
        ns/db/pool/* {
            :title {ns/db/pool/$dbpool}
            :desc {
                The ns/db/pool/$dbpool section configures a global database connection
                pool. A pool defines how NaviServer connects to a database, how many
                handles are maintained, and how idle or stale handles are checked and
                closed.

                Database pools are global and may be used by multiple virtual servers.
                A server selects the pools it uses in ns/server/$server/db.
            }

            :example {
                ns_section ns/db/pool/pool1 {
                    ns_param driver         postgres
                    ns_param datasource     localhost::dbname
                    ns_param user           dbuser
                    ns_param password       secret
                    ns_param connections    15
                    ns_param logminduration 10ms
                }
            }

            driver {
                type string
                desc {Database driver used by this pool}
            }

            datasource {
                type string
                desc {Driver-specific datasource string identifying the database connection target}
            }

            user {
                type string
                desc {Database user name used when opening connections for this pool}
            }

            password {
                type string
                desc {Database password used when opening connections for this pool}
            }

            connections {
                type integer
                default 2
                desc {Number of database connections maintained by this pool}
            }

            checkinterval {
                type time
                default 5m
                desc {Interval at which the pool checks for stale database handles}
            }

            maxidle {
                type time
                default {5m}
                desc {Close database handles that have been idle for at least this interval}
            }

            maxopen {
                type time
                default {60m}
                desc {Close database handles that have been open longer than this interval}
            }

            logminduration {
                type time
                default 0ms
                desc {When SQL logging is enabled, log only statements whose execution time is at least this duration}
            }

            logsqlerrors {
                type boolean
                default false
                desc {Log SQL errors reported by this pool}
            }
        }

        ns/db/drivers {
            :title {ns/db/drivers}
            :desc {
                The ns/db/drivers section defines database driver instances available
                to NaviServer's nsdb interface. Each parameter name is a
                configuration-defined driver name, and each value is the database
                driver module implementation to load.

                Database drivers are external NaviServer modules. They are maintained
                separately from the core server, usually in separate source repositories,
                and must be compiled and installed before they can be loaded. Common
                drivers include nsdbpg for PostgreSQL and nsoracle for Oracle; additional
                drivers are available for other database systems.
            }

            :example {
                ns_section ns/db/drivers {
                    ns_param postgres nsdbpg
                    ns_param nsoracle nsoracle
                }
            }
            :seeIntro {Available external database drivers include}
            :see {
                {uri https://github.com/naviserver-project/nsdbpg nsdbpg}
                {uri https://github.com/naviserver-project/nsoracle nsoracle}
                {uri https://github.com/naviserver-project/nsdbmysql nsdbmysql}
                {uri https://github.com/naviserver-project/nsdbsqlite nsdbsqlite}
                {uri https://github.com/naviserver-project/nsdbtds nsdbtds}
                {uri https://github.com/naviserver-project/nsodbc nsodbc}
                {uri https://github.com/naviserver-project/nsdbbdb nsdbbdb}
            }

            * {
                key {database driver name}
                keySemantics identifier
                type module
                cardinality many
                desc {Configuration-defined database driver name whose value names the installed database driver module implementation; the driver name is used in ns/db/pool/$dbpool via the driver parameter}
            }
        }

        ns/db/driver/* {
            :title {ns/db/driver/$dbdriver}
            :desc {
                The ns/db/driver/$dbdriver section contains optional settings for a
                database driver instance. The final path component is the
                configuration-defined driver name from ns/db/drivers, not necessarily
                the implementation name.

                For example, a configuration may map the driver name postgres to the
                nsdbpg implementation and then configure driver-specific settings
                below ns/db/driver/postgres.
            }

            :example {
                ns_section ns/db/drivers {
                    ns_param postgres nsdbpg
                }

                ns_section ns/db/driver/postgres {
                    ns_param pgbin /usr/lib/postgresql/18/bin/
                }
            }
        }
        ns/db/driver/postgres {
            :title {nsdbpg}
            :desc {
                The nsdbpg module provides the PostgreSQL database driver for the
                nsdb interface. It is an external NaviServer module and must be
                compiled and installed separately.

                The configured driver name is chosen in ns/db/drivers. A common
                convention is to use postgres as the driver name and nsdbpg as the
                implementation.
            }

            :see {
                {uri {https://github.com/naviserver-project/nsdbpg nsdbpg}}
            }

            :example {
                ns_section ns/db/drivers {
                    ns_param postgres nsdbpg
                }

                ns_section ns/db/driver/postgres {
                    ns_param pgbin /usr/lib/postgresql/16/bin/
                }

                ns_section ns/db/pool/pool1 {
                    ns_param driver postgres
                }
            }
            datestyle {
                type enum
                values {
                    ISO SQL POSTGRES GERMAN NONEURO EURO
                    {ISO, MDY} {ISO, DMY} {ISO, YMD}
                    {SQL, MDY} {SQL, DMY}
                    {POSTGRES, MDY} {POSTGRES, DMY}
                    {GERMAN, DMY}
                }
                desc {PostgreSQL DateStyle setting applied to database sessions opened by this driver; explicit values such as ISO, DMY or ISO, MDY are recommended to define both output style and ambiguous date input ordering. The historical values ISO, SQL, POSTGRES, GERMAN, NONEURO, and EURO remain accepted for compatibility. When unset, PGDATESTYLE or the PostgreSQL server default applies}
            }
            pgbin {
                type path
                desc {Directory containing PostgreSQL client tools such as psql; used when these tools are not available on PATH}
            }

        }

        ns/db/driver/nsoracle {
            :title {nsoracle}
            :desc {
                The nsoracle module provides the Oracle database driver for the
                nsdb interface. It is an external NaviServer module and must be
                compiled and installed separately.

                The configured driver name is chosen in ns/db/drivers. A common
                convention is to use nsoracle as both the driver name and the
                implementation name
            }

            :see {
                {uri {https://github.com/naviserver-project/nsoracle nsoracle}}
            }

            :example {
                ns_section ns/db/drivers {
                    ns_param nsoracle nsoracle
                }

                ns_section ns/db/driver/nsoracle {
                    ns_param maxStringLogLength -1
                    ns_param LobBufferSize      32768
                }

                ns_section ns/db/pool/pool1 {
                    ns_param driver nsoracle
                }
            }

            maxstringloglength {
                type integer
                default {-1}
                desc {Maximum length of SQL strings written to the log; -1 disables this limit}
            }

            lobbuffersize {
                type size
                default {32768}
                desc {Buffer size used for Oracle LOB operations}
            }
        }
    }

    modules {

        nssock {
            :title {HTTP Network Driver}
            :scope {global server}
            :desc {
                The nssock module provides the HTTP network driver. It accepts incoming
                TCP connections, parses HTTP requests, and dispatches them to configured
                virtual servers.

                The parameters documented here are the base network-driver parameters.
                They control listen addresses and ports, request size limits, socket
                behavior, keep-alive handling, upload spooling, writer threads, and
                outgoing bandwidth limits. These base parameters are also accepted by
                nsssl, which extends nssock with TLS support.
            }
            :example {
                ns_section ns/modules {
                    ns_param http nssock
                }

                ns_section ns/module/http {
                    ns_param port 8080
                }
            }
            :see {
                {manual {Network Driver Configuration}}
                {module nssock}
            }
            :includeParamDoc network-driver

            acceptsize {
                type integer
                desc {Maximum number of sockets accepted, default to value of backlog}
            }
            address {
                type list
                desc {Local IP address or addresses on which the driver listens; when multiple addresses and ports are specified, NaviServer listens on every address/port combination}
            }
            backlog {
                type integer
                desc {Listen backlog for this driver, defaults to value of listenbacklog}
            }
            bufsize {
                type size
                default 16kB
                desc {Size of I/O buffer for reading requests}
            }
            closewait {
                type time
                default 2s
                desc {Timeout when closing the socket}
            }
            defaultserver {
                type string
                desc {Default virtual server used for requests accepted by this driver when no more specific host or SNI based mapping selects another server}
            }
            deferaccept {
                type boolean
                default false
                desc {Defer accepting a socket until data arrives where supported by the operating system; can improve performance, but may cause recvwait not to apply while the socket remains in the kernel accept queue}
            }
            driverthreads {
                type integer
                default 1
                desc {Number of driver threads; values greater than 1 activate reuseport}
            }
            extraheaders {
                type ns_set
                desc {Additional HTTP response headers sent for responses handled by this network driver; useful for setting driver-wide security headers such as X-Frame-Options, X-Content-Type-Options, and Referrer-Policy}
            }
            hostname {
                type string
                desc {Hostname associated with this network driver; when address is not specified, NaviServer resolves this hostname, or the system hostname when unset, to determine the listen address; when hostname remains unset, the first configured or derived address is used}
            }
            keepalivemaxdownloadsize {
                type size
                default 0MB
                desc {Maximum response size for keep-alive; 0 means no limit}
            }
            keepalivemaxuploadsize {
                type size
                default 0MB
                desc {Maximum upload size for keep-alive; 0 means no limit}
            }
            keepwait {
                type time
                default {5s}
                desc {Keep-alive timeout}
            }

            location {
                type url
                desc {Fallback absolute location URL used when the driver cannot determine a location from a configured host-to-server mapping; normally left unset, since NaviServer computes the location from the request host or local socket address when needed}
            }

            maxheaders {
                type integer
                default {128}
                desc {Maximum number of header lines per request}
            }
            maxinput {
                type size
                default 1MB
                desc {Maximum size for request bodies}
            }
            maxline {
                type size
                default 8kB
                desc {Maximum size of a single header line}
            }
            maxqueuesize {
                type integer
                default {1024}
                desc {
                    Maximum number of preprocessed requests that may
                    remain queued at the network driver level for
                    later queueing attempts. This limit applies to
                    requests whose server and connection pool have
                    typically already been determined, but which could
                    not currently be queued into the selected pool,
                    for example because the pool has no free
                    connection structure and rejectoverrun is disabled.                    
                }
            }
            maxupload {
                type size
                default 0MB
                desc {Spool request bodies larger than this size}
            }
            nodelay {
                type boolean
                default true
                desc {Enable TCP_NODELAY on accepted sockets, disabling Nagle's algorithm; useful for reducing latency, while false leaves Nagle's algorithm enabled}
            }

            port {
                type list
                desc {TCP port or ports on which the driver listens; when multiple addresses and ports are specified, the driver listens on every address/port combination}
            }

            readahead {
                type size
                desc {Amount of extra data to read ahead, defaults to value of bufsize}
            }
            recvwait {
                type time
                default 30s
                desc {Timeout for receiving the full request}
            }
            reuseport {
                type boolean
                default false
                desc {Enable SO_REUSEPORT explicitly}
            }
            sendwait {
                type time
                default {30s}
                desc {Timeout for sending responses}
            }
            sockacceptlog {
                type integer
                default 4
                desc {Log repeated socket accept operations after this threshold; overrides the global ns/parameters sockacceptlog setting for this network driver}
            }
            spoolerthreads {
                type integer
                default 0
                desc {Number of upload spooler threads}
            }
            uploadpath {
                type path
                desc {Directory for temporary upload files; default to value of tmpdir}
            }
            writerbufsize {
                type size
                default {8kB}
                desc {Buffer size for writer threads}
            }

            writerratelimit {
                type integer
                default 0
                desc {Maximum outgoing bandwidth per connection in kilobytes per second for responses sent through writer threads; 0 disables driver-level rate limiting, while limits set per connection or per connection pool override this value}
            }
            writersize {
                type size
                default {1MB}
                desc {Use writer threads for responses larger than this size}
            }
            writerstreaming {
                type boolean
                default false
                desc {Use writer threads for streaming output such as ns_write}
            }
            writerthreads {
                type integer
                default 0
                desc {Number of writer threads for this network driver; 0 disables writer threads}
            }
        }

        nsssl {
            :title {HTTPS Network Driver}
            :scope {global server}
            :desc {
                The nsssl module provides the HTTPS network driver. It extends the
                HTTP driver with TLS support, certificate configuration, client
                certificate verification, OCSP stapling, and optional HTTP/3 Alt-Svc
                advertisement.
            }
            :example {
                ns_section ns/modules {
                    ns_param https nsssl
                }

                ns_section ns/module/https {
                    ns_param port        8443
                    ns_param certificate /usr/local/ns/certificates/server.pem
                }
            }

            :see {
                {manual {TLS Configuration}}
                {module nsssl}
            }
            :include nssock

            certificate {
                type path
                desc {Certificate chain file in PEM format; relative paths are resolved against the certificates directory below the home directory}
            }
            ciphers {
                type string
                desc {OpenSSL cipher list used for TLS connections; controls the allowed cipher suites for protocols where OpenSSL uses the traditional cipher-list syntax}
            }
            ciphersuites {
                type string
                desc {OpenSSL TLS 1.3 cipher suite list for this HTTPS driver; use ciphers for TLS 1.2 and older protocol versions}
            }
            clientcafile {
                type path
                desc {Trusted CA certificates file for client certificate validation; relative paths are resolved against the home directory}
            }
            clientcapath {
                type path
                desc {Trusted CA certificates directory for client certificate validation; relative paths are resolved against the home directory}
            }
            clientcertmode {
                type enum
                values {none request require}
                default {none}
                desc {Client certificate request mode}
            }
            h3advertise {
                type boolean
                default false
                desc {Advertise HTTP/3 availability via the Alt-Svc response header when an HTTP/3 driver is active and linked to this TLS driver; no header is added when Alt-Svc is already set}
            }
            h3persist {
                type boolean
                default false
                desc {Add the persist flag to the HTTP/3 Alt-Svc advertisement, indicating that clients may keep using the advertised HTTP/3 alternative across network changes}
            }
            key {
                type path
                desc {Private key file in PEM format; optional when the private key is included in the certificate file; relative paths are resolved against the certificates directory below the home directory}
            }
            ocspcheckinterval {
                type time
                default {5m}
                desc {OCSP recheck interval}
            }
            ocspstapling {
                type boolean
                default false
                desc {Enable OCSP stapling}
            }
            ocspstaplingverbose {
                type boolean
                default false
                desc {Enable verbose OCSP stapling logging}
            }
            protocols {
                type list
                desc {TLS protocol versions enabled for this driver}
            }

            tlskeyscript {
                type script
                desc {Helper script used to obtain the password for the server private key when the key is encrypted in the PEM file}
            }

            tlskeylogfile {
                type path
                desc {TLS key log file used for writing TLS session secrets for debugging or protocol analysis; when set to a non-empty value, the file is opened in append mode, and when set to an empty value, NaviServer uses the SSLKEYLOGFILE environment variable. The file contains sensitive material that can decrypt captured TLS traffic}
            }
            vhostcertificates {
                type path
                desc {Directory for virtual-host certificates of the default server}
            }
        }

        quic {
            :title {quic}
            :scope global
            :desc {
                The quic module provides the experimental HTTP/3 network driver based
                on QUIC. It is part of NaviServer 5.1, but requires OpenSSL 4.0 or newer.
                The driver is linked to an existing HTTPS driver configuration and
                reuses its TLS certificate, key, and protocol settings.

                HTTP/3 support is disabled by default. The current implementation is
                functional but still experimental and not yet recommended for
                production use. More testing, leak checking, load testing, and
                hardening are still needed.
            }
            :include nsssl

            :example {
                ns_section ns/modules {
                    ns_param https nsssl
                    ns_param h3    quic
                }

                ns_section ns/module/https {
                    ns_param port        8443
                    ns_param certificate /usr/local/ns/certificates/server.pem

                    # Extra parameters specific for HTTP/3
                    # ns_param h3advertise    true
                    # ns_param h3persist      false
                }
                ns_section ns/module/h3 {
                    ns_param https       ns/module/https
                    ns_param recvbufsize 8MB
                    ns_param idletimeout 3s
                    ns_param draintimeout 10ms
                }
            }

            https {
                type section
                default {ns/module/https}
                desc {Configuration section of the HTTPS driver to which this HTTP/3 driver is linked; the QUIC driver reuses the TLS configuration from this section}
            }

            recvbufsize {
                type size
                default {8MB}
                desc {Size of the UDP receive buffer used by the HTTP/3 driver}
            }

            idletimeout {
                type time
                default {3s}
                desc {Maximum idle time for QUIC connections before they are closed}
            }

            draintimeout {
                type time
                default {10ms}
                desc {Drain timeout used when closing QUIC connections, allowing pending packets or connection-close handling to complete}
            }
        }


        nscgi {
            :scope server
            :desc {
                The nscgi module runs external CGI programs for configured URL mappings.
                It maps request methods and URL prefixes to filesystem directories,
                controls the CGI execution environment, and limits request body size and
                execution time.

                The optional interps parameter refers to an ns/interps/$name section that
                maps script filename extensions to interpreter commands. The optional
                environment parameter refers to an ns/environment/$name section whose
                entries are merged into the CGI process environment.
            }
            :see {
                {manual {CGI Configuration}}
                {module nscgi}
            }
            :example {
                ns_section ns/server/$server/modules {
                    ns_param nscgi nscgi
                }

                ns_section ns/interps/cgi {
                    ns_param .pl /usr/bin/perl
                    ns_param .sh /bin/sh
                }

                ns_section ns/environment/cgi {
                    ns_param PATH   /usr/local/bin:/usr/bin:/bin
                    ns_param TMPDIR /tmp
                    ns_param LANG   C.UTF-8
                }

                ns_section ns/server/$server/module/nscgi {
                    ns_param interps     cgi
                    ns_param environment cgi
                    ns_param map         "GET  /cgi-bin /usr/local/ns/cgi-bin"
                    ns_param map         "POST /cgi-bin /usr/local/ns/cgi-bin"
                    ns_param maxinput    1MB
                    ns_param maxwait     30s
                }
            }

            allowstaticresources {
                type boolean
                default false
                desc {Allow CGI requests to serve static resources from the mapped CGI directories; normally disabled so mapped locations are treated as executable CGI resources only}
            }

            environment {
                type string
                desc {Name of an ns/environment/$name section whose entries are merged into the environment of CGI scripts}
            }

            gethostbyaddr {
                type boolean
                default false
                desc {Resolve the remote client address to a hostname for CGI environment variables; disabled by default to avoid DNS lookup overhead and latency}
            }

            interps {
                type string
                desc {Name of an ns/interps/$name section used to map CGI script filename extensions to interpreter commands}
            }

            limit {
                type integer
                default 0
                desc {Maximum number of concurrent CGI executions allowed by this module instance; 0 disables the concurrency limit}
            }

            map {
                type list
                desc {Maps HTTP methods and URL prefixes to filesystem directories containing CGI scripts}
            }

            maxinput {
                type size
                default {1MB}
                desc {Maximum accepted request body size for CGI requests}
            }

            maxwait {
                type time
                default {30s}
                desc {Maximum time to wait for a CGI process to complete before treating the request as failed}
            }

            systemenvironment {
                type boolean
                default false
                desc {Pass the server process environment to CGI scripts; disabled by default to avoid exposing unintended environment variables}
            }
        }

        nscp {
            :title {nscp}
            :scope server
            :desc {
                The nscp module provides the NaviServer control port.  It listens on a
                configured local address and port and should normally be restricted to
                trusted interfaces such as loopback.
            }
            :example {
                ns_section ns/server/$server/modules {
                    ns_param nscp nscp
                }

                ns_section ns/server/$server/module/nscp {
                    ns_param address 127.0.0.1
                    ns_param port    9999
                }

                ns_section ns/server/$server/module/nscp/users {
                    #ns_param user "username:password:"
                }
            }

            address {
                type address
                desc {Local address on which the control port listens; should normally be restricted to a trusted interface such as loopback (e.g. 127.0.0.1)}
            }

            port {
                type integer
                desc {TCP port on which the control port listens}
            }
        }

        nslog {
            :title {nslog}
            :scope server
            :desc {
                The nslog module writes HTTP access logs for a virtual server.  It can
                produce combined access logs, rotate log files, include request timing
                and thread information, and optionally anonymize logged client
                addresses.
            }
            :title0 {Access Logging Module}
            :see {
                {manual {Access Logging}}
                {module nslog}
            }
            :example {
                ns_section ns/server/$server/modules {
                    ns_param nslog nslog
                }

                ns_section ns/server/$server/module/nslog {
                    ns_param file        access.log
                    ns_param rolllog     true
                    ns_param maxbackup   10
                    ns_param logcombined true
                }
            }

            checkforproxy {
                type boolean
                default false
                deprecated {Use ns/parameters/reverseproxymode instead}
                desc {Legacy option for logging the client address from X-Forwarded-For; deprecated in favor of reverse-proxy mode, which centralizes trusted proxy handling for access logs and peer-address APIs}
            }

            driver {
                type pattern
                default {}
                desc {Tcl string-match pattern for selecting network driver instance names to be logged by this nslog instance; when unset, the access log records requests from all drivers}
            }

            extendedheaders {
                type list
                desc {Additional header fields to include in access log entries; entries without a prefix are treated as request headers, while request:NAME and response:NAME select request and response headers explicitly}
            }

            file {
                type path
                default {access.log}
                desc {Access log file written by this nslog instance; relative paths are resolved against the server log directory}
            }

            formattedtime {
                type boolean
                default true
                desc {Use formatted timestamps instead of Unix time}
            }

            logpartialtimes {
                type boolean
                default false
                desc {Log partial request timing information for selected request processing phases; useful for diagnosing where request time is spent}
            }

            logreqtime {
                type boolean
                default false
                desc {Include total request processing time in access log entries}
            }

            logthreadname {
                type boolean
                default false
                desc {Include the connection thread name in access log entries; useful for debugging thread and pool behavior}
            }

            logcombined {
                type boolean
                default true
                desc {Use NCSA combined log format including referer and user-agent}
            }
            maskipv4 {
                type ip-mask
                desc {IPv4 address mask for anonymizing logged client addresses}
            }
            maskipv6 {
                type ip-mask
                desc {IPv6 address mask for anonymizing logged client addresses}
            }
            masklogaddr {
                type boolean
                default false
                desc {Anonymize logged client addresses using maskipv4 and maskipv6 before writing access log entries}
            }
            maxbackup {
                type integer
                default 100
                desc {Maximum number of rotated log files}
            }
            maxbuffer {
                type integer
                default 0
                desc {Number of log entries buffered}
            }
            rollfmt {
                type string
                desc {Suffix format used when rotating the access log file; controls how timestamps are appended to rolled log filenames}
            }
            rollhour {
                type integer
                default 0
                desc {Hour of day to roll the log}
            }
            rolllog {
                type boolean
                default true
                desc {Rotate logs automatically}
            }
            rollonsignal {
                type boolean
                default false
                desc {Rotate log on SIGHUP}
            }

            suppressquery {
                type boolean
                default false
                desc {Suppress the query string in logged request URLs; useful for reducing log volume and avoiding accidental logging of sensitive URL parameters}
            }
        }

        nsperm {
            :title {nsperm}
            :scope server
            :desc {
                The nsperm module provides file-based permission checking for a virtual
                server. It can protect URL spaces using access-control data and optional
                .htaccess-style files.

                This module is mainly useful for simple deployments or compatibility
                with older configurations. Applications with their own authentication
                and authorization layer typically use application-level permission
                checks instead.
            }

            :example {
                ns_section ns/server/$server/modules {
                    ns_param nsperm nsperm
                }

                ns_section ns/server/$server/module/nsperm {
                    ns_param htaccess false
                }
            }
            htaccess {
                type boolean
                default false
                desc {Activate htaccess mode, which is similar to Apache but more simpler and limited in functionality. It supports only allowing and denying access to a particular directory.}
            }
            passwdfile {
                type path
                default {HOME/modules/nsperm/passwd}
                desc {Absolute path to the passwd file}
            }
        }

        nsproxy {
            :title {nsproxy}
            :scope server
            :desc {
                The nsproxy module manages pools of external worker processes used to
                evaluate Tcl code outside the main NaviServer process. This can be used
                to isolate potentially unsafe, blocking, or resource-intensive work
                from the server process.

                Each nsproxy module instance configures a worker pool, including the
                worker executable, timeout behavior, maximum number of workers, and
                logging threshold for long-running operations.
            }

            :example {
                ns_section ns/server/$server/modules {
                    ns_param nsproxy nsproxy
                }

                ns_section ns/server/$server/module/nsproxy {
                    ns_param exec           /usr/local/ns/bin/nsproxy
                    ns_param maxworker      8
                    ns_param gettimeout     0ms
                    ns_param evaltimeout    0ms
                    ns_param sendtimeout    5s
                    ns_param recvtimeout    5s
                    ns_param waittimeout    1s
                    ns_param idletimeout    5m
                    ns_param logminduration 1s
                }
            }

            exec {
                type path
                desc {Worker executable used for nsproxy processes; when unset, NaviServer uses the default nsproxy executable}
            }

            gettimeout {
                type time
                default {0ms}
                desc {Timeout for obtaining an nsproxy worker from the pool; 0 disables this timeout}
            }

            evaltimeout {
                type time
                default {0ms}
                desc {Timeout for evaluating a request in an nsproxy worker; 0 disables this timeout}
            }

            sendtimeout {
                type time
                default {5s}
                desc {Timeout for sending data or commands to an nsproxy worker}
            }

            recvtimeout {
                type time
                default {5s}
                desc {Timeout for receiving data or results from an nsproxy worker}
            }

            waittimeout {
                type time
                default {1s}
                desc {Timeout while waiting for an nsproxy worker process to become ready or complete a control operation}
            }

            idletimeout {
                type time
                default {5m}
                desc {Maximum idle time for an nsproxy worker before it may be closed}
            }

            maxworker {
                type integer
                default {8}
                desc {Maximum number of nsproxy worker processes in this pool}
            }

            maxslaves {
                type integer
                default {8}
                deprecated {Use maxworker instead}
                desc {Legacy name for maxworker}
            }

            logminduration {
                type time
                default {1s}
                desc {Log nsproxy operations whose duration is at least this threshold}
            }
        }

        nsdb {
            :title {nsdb}
            :scope server
            :desc {
                The nsdb module provides NaviServer's database integration for a
                virtual server. Loading this module enables the server to use database
                pools defined globally under ns/db/pools and ns/db/pool/$dbpool.

                The server-specific ns/server/$server/db section selects which global
                pools are available to this server and which one is used as the default
                pool.
            }

            :example {
                ns_section ns/server/$server/modules {
                    ns_param nsdb nsdb
                }

                ns_section ns/server/$server/db {
                    ns_param pools       pool1,pool2,pool3
                    ns_param defaultpool pool1
                }
            }
        }

        nssmtpd {
            :scope global
            :desc {
                The nssmtpd module provides an SMTP server for NaviServer. It can be
                used to receive mail locally, relay messages to another mail server,
                filter incoming messages, and dispatch SMTP processing to Tcl callback
                procedures.

                The module is an external NaviServer module and must be compiled and
                installed separately. For new mail-related applications, nssmtpd or
                Tcl library based mail support is usually preferable to the legacy
                ns_sendmail command.

                The logging parameters documented here apply to SMTP sending operations
                performed by the module. A graphical viewer for the log file is included
                in the nsstats module.
            }
            :see {
                {uri https://github.com/naviserver-project/nssmtpd nssmtpd}
            }
            :example {
                ns_section ns/server/$server/modules {
                    ns_param nssmtpd nssmtpd
                }

                ns_section ns/server/$server/module/nssmtpd {
                    ns_param port        2525
                    ns_param address     127.0.0.1
                    ns_param relay       localhost:25
                    ns_param spamd       localhost

                    ns_param initproc    smtpd::init
                    ns_param rcptproc    smtpd::rcpt
                    ns_param dataproc    smtpd::data
                    ns_param errorproc   smtpd::error

                    ns_param relaydomains "localhost"
                    ns_param localdomains "localhost"

                    ns_param logging     on
                    ns_param logfile     smtpsend.log
                    ns_param logrollfmt  %Y-%m-%d
                }
            }

            address {
                type address
                desc {Local address on which the SMTP server listens}
            }

            port {
                type integer
                desc {TCP port on which the SMTP server listens}
            }

            relay {
                type address
                desc {Upstream SMTP server or mail relay used for forwarding messages}
            }

            spamd {
                type address
                desc {Spamd or SpamAssassin daemon used for filtering messages}
            }

            initproc {
                type proc
                desc {Tcl callback procedure invoked during SMTP session initialization}
            }

            rcptproc {
                type proc
                desc {Tcl callback procedure invoked for SMTP recipient handling}
            }

            dataproc {
                type proc
                desc {Tcl callback procedure invoked for processing SMTP message data}
            }

            errorproc {
                type proc
                desc {Tcl callback procedure invoked for SMTP error handling}
            }

            relaydomains {
                type list
                desc {Domain names for which this SMTP server accepts mail for relaying}
            }

            localdomains {
                type list
                desc {Domain names treated as local by this SMTP server}
            }

            logging {
                type boolean
                default {false}
                desc {Enable logging for SMTP sending operations performed by the module}
            }

            logfile {
                type path
                desc {Log file for SMTP sending operations; relative paths are resolved according to the module's logging rules}
            }

            logmaxbackup {
                type integer
                default {10}
                desc {Maximum number of rotated SMTP sending log files to keep}
            }

            logroll {
                type boolean
                default {true}
                desc {Enable automatic rotation of the SMTP sending log}
            }

            logrollfmt {
                type string
                desc {Suffix format used when rotating the SMTP sending log file}
            }

            logrollhour {
                type integer
                default {0}
                desc {Hour of day at which to rotate the SMTP sending log}
            }

            logrollonsignal {
                type boolean
                default {false}
                desc {Rotate the SMTP sending log when the server receives a SIGHUP signal}
            }

            certificate {
                type path
                desc {Certificate chain file used for STARTTLS support}
            }

            cafile {
                type path
                desc {CA certificate file used for TLS certificate validation}
            }

            capath {
                type path
                desc {Directory containing CA certificates used for TLS certificate validation}
            }

            ciphers {
                type string
                desc {OpenSSL cipher list for TLS 1.2 and older STARTTLS connections}
            }
        }

        revproxy {
            :title {Reverse Proxy Modules}
            :scope server
            :desc {
                The revproxy module provides reverse-proxy support for a virtual
                server. It forwards selected incoming requests to configured backend
                servers and returns the backend response to the client.

                Backend servers are configured in subsections below the revproxy
                module configuration. The main module section contains global defaults
                and initialization hooks, while each backend subsection defines its
                target URLs, URL mappings, timeouts, optional URL rewriting, backend
                connection handling, and forwarding constraints.

                Backend subsections are documented separately in the following revproxy
                backends subsection.
            }

            :example {
                ns_section ns/server/$server/modules {
                    ns_param revproxy tcl
                }

                ns_section ns/server/$server/module/revproxy {
                    ns_param verbose false
                }

                ns_section ns/server/$server/module/revproxy/backend1 {
                    ns_param target https://backend.example.com/
                    ns_param map    "GET  /api/*"
                    ns_param map    "POST /api/*"
                }
            }

            verbose {
                type boolean
                default false
                desc {Enable verbose logging for reverse proxy operations in the system log}
            }
            backendconnection {
                type enum
                values {s_http+ns_connchan ns_http ns_connchan}
                default ns_http+ns_connchan
                desc {Default backend connection type used for reverse proxy backends; can be overridden per backend}
            }

            register {
                type script
                desc {Tcl script evaluated during revproxy initialization; used to register additional handlers or filters for URLs handled by the proxy itself rather than forwarded to a backend}
            }
        }

        nsstats {
            :title {nsstats}
            :scope global
            :desc {
                The nsstats module provides status and diagnostic pages for a running
                NaviServer instance. It is configured as a global Tcl module and can
                display runtime information such as server statistics, locks, threads,
                loaded modules, and the current configuration database.

                This module provides access to potentially sensitive
                information. Do not deploy it on public-facing
                websites unless you implement appropriate access
                controls. For OpenACS installations, it is recommended
                to place the module main script nsstats.tcl in a
                secure directory such as acs-subsite/www/admin/,
                ensuring that only authorized users can access it.
            }
            :see {
                {uri https://github.com/naviserver-project/nsstats nsstats}
            }

            :example {
                ns_section ns/module/nsstats {
                    ns_param enabled  true
                    ns_param user     nsadmin
                    ns_param password ""
                    ns_param bglocks  {oacs:sched_procs}
                }
            }

            enabled {
                type boolean
                default {true}
                desc {Enable or disable the nsstats module}
            }

            user {
                type string
                default {nsadmin}
                desc {User name required for accessing nsstats pages}
            }

            password {
                type password
                default {}
                desc {Password required for accessing nsstats pages; when empty, access control depends on the surrounding configuration}
            }

            bglocks {
                type list
                default {}
                desc {List of lock names treated as non-per-request background locks in nsstats lock reporting}
            }
        }

    }
}
