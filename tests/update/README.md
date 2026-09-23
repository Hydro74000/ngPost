# Release update validation

`Update process` runs on Linux, Windows/MSVC and macOS on pushes and PRs. It is
also a required dependency of `Build and Release`. Its tests run sequentially:

- `tst_UpdateChecker`: release channels, real release JSON parsing, platform asset
  names and URLs, notification, portable/source/Setup/AppImage policy, download
  bounds and cancellation. Test-count floors cover all three OSes.
- `test_update_e2e.py`: native executables and real Python installer subprocesses;
  wait for the previous process, swap, restart, next update, cancellation,
  corrupted download, failed preflight and rollback after a failed installed probe.
- `test_update_handoff.py`: C++ notification through download of manifest/archive,
  embedded Python extraction and verification, detached installer readiness,
  application exit and restart, including launching from the installation directory
  (which otherwise pins the Windows directory during rename). Only HTTP transport is substituted, with the
  production HTTPS URLs. No live release needs publishing to exercise this path.

For a complete update rehearsal without publishing, dispatch `Build and Release`
with `updates_only=true`, `publish=false` and the prospective stable version
(e.g. `v5.6`). This runs the native update tests, GUI tests and all package builds,
including legacy and Setup upgrades. Unrelated posting/VPN/full unit suites are
omitted in this explicit rehearsal mode. The workflow refuses publication in
that mode; ordinary release runs retain every existing prerequisite.

Each release archive must also pass `check_release_package.py` on its own OS
before artifact upload. This uses the actual archive and the production manifest
writer, starts the packaged GUI, waits for its exit, swaps the installation,
checks the installed bytes and TLS, observes GUI restart, and prepares another
update. It preserves a sibling profile. This tests a package replacement round
trip, not migration from every historical ngPost configuration schema.

`build_legacy_client.py` compiles the unmodified updater sources from stable 5.5.1
(commit `e5cc72a8451f1d44891c347ea1cf030ff5ddf538`). Only its coordinator dependency
and HTTP transport are fixtures. `check_legacy_package.py` runs that code against
the candidate archive and checks notification, replacement and GUI restart.
The candidate is advertised as stable `v99.0` so unstable CI builds can be tested
without violating the old client's stable-channel policy. Archive bytes remain
unchanged. These tests use the old client's fixed temporary paths; run only
sequentially on disposable machines. Existing fixed paths cause a refusal.

Windows release CI additionally installs the real 5.5.1 Setup, upgrades using the
candidate Setup, checks TLS, preserved configuration, optional ParPar and the
uninstaller, then uninstalls. This test deliberately refuses local execution
because Inno Setup writes the production AppId to the registry.

| Installation | Notification | Replacement supported by 5.6 |
| --- | --- | --- |
| Linux x86_64 portable tar.gz, system Qt | In-app | Owned, writable installation; Python 3.9+ |
| Windows x86_64 portable ZIP, bundled Qt | In-app | Owned, writable installation; Python 3.9+ |
| macOS bundled .app ZIP | In-app | Owned, writable bundle; Python 3.9+ |
| Windows Setup | Red status-bar release link | Run the new Setup; retains installer state |
| Linux AppImage | Red status-bar release link | Download replacement / external AppImage updater; existing AppImage smoke and zsync metadata checks remain release gates |
| Source/build trees (including legacy copied markers), unbundled macOS executable, system-managed directories | Red status-bar release link (GUI) | Rebuild or use the package manager; no directory replacement |
| Headless / Docker | No interactive updater | Rebuild / pull container using the existing container CI |

No update is offered to a stable build for a prerelease. Stable supersedes an
unstable of the same version; newer unstable builds remain discoverable by
unstable builds. AppImage versions already shipped with checks disabled cannot
be made to display the new notification retroactively.

The Windows journal flush regression exposed by these tests prevents the old
SHA-256 installer from reaching readiness on Windows. It is fixed in the current
installer. Already-installed unstable builds embedding that broken installer
need a manual ZIP/Setup upgrade; a downloaded release cannot repair the verifier
already running in the old process. Stable 5.5.1 uses the separate legacy path.

Local Linux reproduction (inside `my-distrobox`):

```sh
c++ -std=c++17 tests/update/update_fixture.cpp -o /tmp/update-fixture
export NGPOST_UPDATE_FIXTURE=/tmp/update-fixture
export NGPOST_UPDATE_CLIENT="$PWD/tests/unit/tst_UpdateChecker/tst_UpdateChecker"
python3 tests/update/test_update_e2e.py -v
python3 tests/update/test_update_handoff.py -v
python3 tests/update/check_release_package.py --archive /path/ngPost-v5.6-linux-x86_64.tar.gz --tag v5.6
```
