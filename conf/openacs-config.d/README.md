# openacs-config.d – configuration fragments

This directory contains the fragment-based form of the OpenACS sample
configuration.  NaviServer can load configuration from either a single
file or a directory of `*.tcl` fragments (loaded in lexicographic order):

- single file: `nsd -t conf/openacs-config.tcl ...`
- fragments:   `nsd -t conf/openacs-config.d   ...`

The fragments are organized into numbered sections (00, 10, 20, …, 70)
to provide a consistent structure across sample configurations.
Concatenating the fragments in order yields the same behavior as the
single-file configuration.

## Module activation design (OpenACS sample)

The OpenACS sample configuration uses a simple, Docker-friendly design
for selecting which modules are loaded.  This is a convention used by
this configuration; it is not mandatory for NaviServer configurations in
general.

Modules fall into three categories:

### 1) Unconditional modules (always loaded)

These modules are always configured/loaded because they are expected for
typical OpenACS installations.

Examples:
- `nsdb`   – database driver integration
- `nslog`  – access logging
- `nsproxy` – process proxy support
- `libthread` – Tcl thread library integration

Note: For `libthread` the configuration attempts to locate an installed
library under `$homedir/lib`.  If no library is found, an error is
logged.

### 2) Auto-enabled modules (enabled when sufficiently configured)

These modules are enabled automatically when the configuration provides
the minimal required parameters.  This allows simple enable/disable via
environment variables in containerized deployments.

Examples:
- `nscp`     – enabled when `nscpport` is non-empty
- `nssmtpd`  – enabled when `smtpdport` is non-empty
- `letsencrypt` – enabled when `letsencrypt_domains` is non-empty
- network drivers – enabled when `httpport` / `httpsport` are configured

### 3) Opt-in modules (enabled via `servermodules`)

Some modules are optional and are only enabled when explicitly listed in
the configration variable `servermodules` (a Tcl list) defined in the 
00-* section.  This is intended for features that do not have a natural 
“required parameter” trigger, or that are not needed on every site.

Examples:
- `nscgi`
- `nsshell`
- `nspam`
- `websocket`
- `nswebpush`

Enable example (in a setup file or config fragment):

```tcl
set servermodules {nscgi websocket}
```

## Variables and Docker-style overrides

The OpenACS sample config defines defaults in `00-bootstrap.tcl` and then
applies overrides using `ns_configure_variables`, which supports
environment variables with a prefix (e.g. `oacs_`).

Example:

```sh
oacs_httpport=8080 oacs_httpsport=8443 nsd -t conf/openacs-config.d -f
```

Optional instance-specific settings can be placed in a `setupfile`
sourced early by the bootstrap logic (see `00-bootstrap.tcl`).

## Notes

* The fragment structure is an ordering recommendation.  Only the
  bootstrap fragment must run early, and final diagnostics run late.
* The OpenACS-specific package configuration lives under
  `ns/server/$server/acs` and is grouped in dedicated fragments.


