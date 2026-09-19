# Security policy

ngPost runs code with elevated privileges on two platforms: a privileged VPN
helper started through a polkit rule on Linux, and PowerShell scripts run under
UAC on Windows. It also stores Usenet, archive and VPN credentials, and it can
update itself. A vulnerability in any of these deserves a private report rather
than a public issue.

## Supported versions

| Version | Supported |
| --- | --- |
| Latest stable release (currently 5.5.x) | Yes |
| Latest unstable build of `devel` (`v5.6-unstable.*`) | Yes |
| Any older release or unstable build | No: update first |

This covers every package of those versions: Windows installer and portable
zip, macOS zip, Linux archive and AppImage. From the next release on, it also
covers the container image published on ghcr.io, where `latest` follows the
stable release.

How a version reaches users:

- the in-app update check offers stable builds the latest stable release only,
  and unstable builds a newer unstable build or the stable release that
  supersedes them;
- it installs the update itself only for a packaged installation, and only when
  Python 3.9 or later is available; otherwise it asks for a manual install
  after checking the published SHA-256;
- the AppImage skips that check: it carries zsync update information for
  AppImage update tools instead.

## Reporting a vulnerability

Report it privately through GitHub:
**[Security → Report a vulnerability](https://github.com/Hydro74000/ngPost/security/advisories/new)**.
The form needs a GitHub account; only you and the maintainers can read what you
send.

Please do not open a public issue, pull request or discussion for it.

A useful report says:

- the ngPost version (`ngPost --version`), the package and the operating system;
- what an attacker needs beforehand: a local account, a crafted file, a
  malicious server or network position, a modified configuration;
- what they gain;
- the steps to reproduce it, ideally with a minimal configuration or input file.

Remove your real server credentials from anything you attach.

## What happens next

1. **Acknowledgement within 7 days.** The report is confirmed or questioned in
   the advisory itself.
2. **Fix within 30 days of confirmation.** A confirmed vulnerability is fixed
   and released in **both** supported lines within 30 days, without waiting for
   the batch of changes in progress. If that cannot be met, the advisory says
   why and gives a new date.
3. **Coordinated disclosure.** The advisory is published when the fixed
   releases are out, with a CVE when one applies. Reporters are credited unless
   they ask not to be.

## How security fixes are made

Two long-lived branches carry security fixes, so an urgent fix never has to
wait for, or ship with, unrelated work:

| Branch | Based on | Released through |
| --- | --- | --- |
| `stable-security` | `master` | merged into `master`: a stable release |
| `devel-security` | `devel` | merged into `devel`: an unstable build |

A fix is written once, ported to the other branch, and released on both lines
together:

1. On `stable-security`, raise `VERSION` in `src/ngPost_core.pri` (5.5.1 to
   5.5.2, for instance). A stable release is tagged `v<VERSION>`: with an
   unchanged number, the release workflow would update the existing release and
   replace its files, and the in-app update check would offer the fix to no one.
   Unstable builds need no such step: their tag carries the date and the build
   number.
2. Merge each branch through a pull request, into `master` and into `devel`.
   The test workflows run on every push to these branches and on the pull
   request; the container image checks run on the pull request as well when
   the fix touches what the image is built from.
3. The release workflow publishes only from `master` and `devel`, never from
   the security branches.

This repository is public, so anything pushed to these branches is public too.
A fix for a vulnerability that is not yet disclosed is developed in the
**temporary private fork** of its GitHub security advisory, and is pushed to
`stable-security` and `devel-security` only when the releases are ready to go
out.

## Scope

In scope, among others:

- the Linux privileged VPN helper, its installer and its polkit rule
  (`src/vpn/scripts/`, `src/vpn/polkit/`);
- the Windows scripts run under UAC and their integrity checks
  (`src/vpn/scripts/win/`, `src/vpn/WindowsSecurity*`);
- the sanitisation of OpenVPN and WireGuard profiles;
- the update mechanism (`src/utils/UpdateChecker*`,
  `src/utils/update/install_update.py`);
- the storage of credentials and configuration (`src/utils/PathHelper*`), and
  the masking of passwords in logs (`src/utils/SecretMasker*`);
- NNTP and TLS handling, and the parsing of NZB files and server replies;
- the commands and uploads run after a post (`NZB_POST_CMD`, `NZB_UPLOAD_URL`);
- the published packages and the pipeline that builds them (`.github/`,
  `Dockerfile`): what they contain, and what they let a local user change.

Out of scope:

- vulnerabilities in the third-party tools ngPost runs or bundles (rar, 7-Zip,
  par2cmdline, ParPar, MultiPar, OpenVPN, WireGuard): report them to their
  authors. Report it here if the way ngPost bundles or calls them is at fault;
- attacks that require an administrator or root account already;
- the behaviour of Usenet providers.

## Known limitation

Automatic updates check the SHA-256 of each downloaded file against a manifest
published with the release. They do not verify a publisher signature: the
authenticity of an update rests on HTTPS and on the GitHub release itself.
