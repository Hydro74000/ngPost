# SHA-256 update contract (unsigned releases)

Automatic updates require Python 3.9+ on the client. Missing Python or an installation without the
`.ngpost-installation` package marker cause an explicit failure before mutation.
Linux system directories and package-manager installations use manual updates.
AppImage remains on its separate update path.

No certificate, key or OpenSSL executable is required by the updater. Qt still
uses its TLS backend for HTTPS; this change does not disable transport security.

Each release contains `manifest.json` and `SHA256SUMS` (not signed).
The manifest is UTF-8 JSON with `schema: 1`,
`tag` and `assets`, an array of `{name, size, sha256}` records. The updater
checks the tag, exact asset name, size and SHA-256 before extraction.
Malformed hashes, missing metadata and duplicate JSON fields/assets fail closed.
`SHA256SUMS` provides the same hashes for manual use and is reproduced in the
GitHub release body. Hashes establish integrity against GitHub metadata, not
independent publisher authenticity. A compromise replacing both can bypass this
check. Unsigned releases are an explicit maintainer decision.

Downloads use HTTPS with an exact GitHub host allowlist, validated redirects,
30-second transfer timeouts and fixed metadata/asset limits. Files are streamed
to a private random directory beside the installation. Extraction rejects
traversal, ambiguous Windows names, duplicates, devices and hard links. Internal
symbolic links are created last and their complete resolution must stay inside
the extraction directory. Expanded bytes and member counts are bounded. ZIP
central-directory metadata is capped before ZipFile allocates it; TAR extended
headers are capped before tarfile processes them. ZIP64 and GNU sparse metadata
requiring additional allocations are unsupported. The tarfile processing hook
is covered by regression tests and must be revalidated on Python upgrades.

The checksum-verified candidate must pass `--version` before the application exits.
Linux/macOS swap directories atomically using renameat2/renamex_np; unsupported
filesystems fail without changing the installation. Windows uses two journaled
directory renames after the application exits; **this is recoverable, not a
single atomic directory exchange**. A failed second rename immediately restores
the previous directory. A failed post-swap probe or launch rolls back. A power
loss between Windows renames requires recovery from the retained journal.

The private `.ngpost-update-*` directory retains `transaction.json`, the previous
installation (`previous` on Windows, `candidate` path in the journal on POSIX),
and `error.txt` on failure. No updater recursively removes an installation.
For manual recovery, close ngPost, retain the failed tree elsewhere and rename
the previous tree back to the journal's `install` path. The installer waits for
the parent process to exit; Cancel aborts network/preparation and leaves a
cancellation marker checked before replacement.
Late callbacks from canceled attempts cannot affect a retry. Programmatic
closure of the progress dialog and normal application destruction after a
successful handoff do not cancel the detached transaction.

Native Windows/macOS execution must still be validated in CI. No platform
signing identities or signing approvals are required for publication.
