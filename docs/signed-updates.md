# Signed update contract

Automatic updates require Python 3.9+ and the OpenSSL command-line tool on the
client. Missing tools, an unprovisioned key or an installation without the
`.ngpost-installation` package marker cause an explicit failure before mutation.
Linux system directories and package-manager installations use manual updates.
AppImage remains on its separate update path.

Provision an RSA public key (at least 3072 bits) in
`src/utils/update/update-key.pem`, review it and commit it before publishing.
The matching private PEM belongs only in the protected release environment's
`RELEASE_SIGNING_KEY` secret. Never download a verification key from the release
being verified. Rotate keys through a release authenticated by the existing key.

Each release contains `manifest.json` and its binary OpenSSL RSA/SHA-256
signature `manifest.json.sig`. The manifest is UTF-8 JSON with `schema: 1`,
`tag` and `assets`, an array of `{name, size, sha256}` records. The updater
checks the signed tag, exact asset name, size and SHA-256 before extraction.
`SHA256SUMS` and `SHA256SUMS.sig` provide the same verification for manual use.

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

The signed candidate must pass `--version` before the application exits.
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

Native Windows/macOS execution and publication credentials must be validated in
CI before a public release. An empty key deliberately disables automatic update
installation; it is not a trust-on-first-use mechanism.
