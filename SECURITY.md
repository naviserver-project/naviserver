# Security Policy

## Reporting a Vulnerability

Please report suspected security vulnerabilities privately through
[GitHub private vulnerability reporting](https://github.com/naviserver-project/naviserver/security/advisories/new).

If that channel is unavailable, contact [SECURITY CONTACT EMAIL].

Please do not include undisclosed vulnerability details in public issues,
pull requests, or mailing lists. Ordinary bugs and questions can be
reported through the project's usual public channels.

## Supported Versions

Security fixes target the latest patch release of each maintained line.

| Release line | Security maintenance |
|--------------|----------------------|
| 5.1.x        | Supported |
| 5.0.x        | Supported maintenance branch |
| 4.99.x       | Selected security fixes on a best-effort basis |
| Older lines  | No planned security updates |

The `main` branch contains ongoing development and is not a stable release.
Reports affecting development versions are welcome.

Please use the latest patch release available for your release line.
Reports involving older versions are still useful when the underlying
problem may affect a maintained version.

## What to Include

Please provide, where available:

- The NaviServer version, release tag, or commit.
- Operating system, architecture, and relevant dependency versions,
  including Tcl, OpenSSL or LibreSSL, and nghttp3.
- Affected modules, configuration options, and deployment prerequisites.
- Reproduction steps or a minimal test case.
- Expected and observed behavior.
- The security impact and the access or privileges required to trigger it.
- Relevant logs, crash traces, or sanitizer output.

A working exploit is not required. Please remove credentials, private
keys, personal data, and other sensitive information from reports.

## Scope and Security Boundaries

Reports concerning NaviServer core and components shipped in this
repository are welcome. Examples include memory-safety errors, request
framing inconsistencies, authorization bypasses, unintended file access,
injection, sensitive-data exposure, and resource exhaustion.

Please describe how an attacker can reach the affected behavior.
Non-default configurations and optional modules are not automatically
out of scope.

Server configuration, application Tcl scripts, and native extensions
execute with server authority. NaviServer does not provide isolation
between mutually untrusted applications within one process. Intended
execution of trusted application code is not itself a vulnerability;
a flaw triggered through normal use of an application API may be.

Reports about experimental features are welcome. Their experimental
status does not exclude security problems, but fixes and compatibility
may follow the development schedule.

Separately maintained NaviServer modules have their own maintainers.
Please report vulnerabilities in those modules to the responsible
project and notify NaviServer maintainers when coordination is needed.

## External Components

OpenSSL, LibreSSL, Tcl, nghttp3, database client libraries, and other
third-party dependencies are maintained by their respective projects.
Vulnerabilities originating solely in those components should normally
be reported through the upstream project's security reporting process.

NaviServer's integration with these components remains in scope,
including incorrect API use, unsafe configuration defaults, missing
validation, and failures to enforce NaviServer's security guarantees.

Please also notify NaviServer maintainers when a dependency vulnerability
affects supported NaviServer deployments or requires changes to
NaviServer's code, build requirements, configuration, or documentation.
We will coordinate with upstream maintainers where appropriate.

## Response and Coordinated Disclosure

We aim to acknowledge private reports within seven business days.
Please include any disclosure deadline or organizational reporting
requirements with your initial report.

We will assess the report, discuss affected versions and potential
mitigations, and agree on a proposed disclosure date with the reporter.
A coordination period of 90 calendar days from the initial private
report is a usual starting point. Different timelines may be appropriate
depending on severity, active exploitation, dependency coordination,
and the availability of fixes.

NaviServer is maintained by volunteers. We cannot guarantee remediation
within a fixed period, but will communicate progress and discuss any
necessary changes to the proposed timeline before it expires.

We do not request indefinite confidentiality or make publication
conditional on maintainer approval. If a fix is not available by the
disclosure date, we will seek to coordinate an advisory describing the
known impact and available mitigations.

Active exploitation or an already-public vulnerability may require
earlier disclosure. We will coordinate the release of fixes, advisories,
and technical details where possible, with user protection as the
priority. Because public source commits can reveal a vulnerability,
coordination should also cover the publication of fixing commits
and relevant backports.

All fixed security issues will be disclosed in the release notes for
each release that includes the fix, including patch releases and
releases containing backports. The release notes will link to the
corresponding security advisory when one is available.

Security advisories describe the impact, affected versions, available
fixes, and relevant mitigations. Advisories may be published before a
release when users need earlier notice. Reporter credit will be included
with the reporter's agreement.

## Testing Guidelines

Please conduct testing only on systems you own or are authorized to
test. Avoid accessing other people's data, disrupting services, or
conducting destructive tests against production installations.

Where possible, demonstrate the issue using a local test environment
and the minimum input necessary to establish its impact.
