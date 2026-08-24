//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef PATHHELPER_H
#define PATHHELPER_H

#include <QString>
#include <QStringList>

//! Cross-platform paths for ngPost configuration and runtime data.
//!
//! Resolution — the final segment is the literal "ngPost", never a name
//! derived from the executable (see configDir() for why that matters):
//!  - Linux  : $XDG_CONFIG_HOME/ngPost/   (fallback ~/.config/ngPost/)
//!  - Windows: %LOCALAPPDATA%/ngPost/     (C:/Users/<user>/AppData/Local/ngPost)
//!  - macOS  : ~/Library/Application Support/ngPost/
//!
//! Layout:
//!  <configDir>/
//!     ngPost.conf      main configuration (was ~/.ngPost on Linux/macOS
//!                      and ngPost.conf next to the executable on Windows)
//!     ngPost_gui.ini   GUI geometry / column widths
//!     ngPost_history.sqlite  structured post history (unless POST_DB says
//!                      otherwise in ngPost.conf)
//!     *.txt            post-info templates named by POST_INFO_TEMPLATE, which
//!                      resolves relative to this folder
//!     vpn/             per-user VPN profile files (.ovpn / .conf)
//!     vpn/runtime/     short-lived auth files (chmod 600) — only while a
//!                      VPN connection is active
//!     .ngPost_config_migration
//!                      written once if this folder was adopted from an older,
//!                      name-derived one — see migrateAppNamedConfigDirIfNeeded()
namespace PathHelper
{
enum class ConfigMigrationStatus
{
    NoLegacy,
    AlreadyMigrated,
    CopiedAndKeptLegacy,
    SkippedNewExists,
    NeedsOverwriteConfirmation,
    BackupFailed,
    CopyFailed
};

struct ConfigMigrationResult
{
    ConfigMigrationStatus status = ConfigMigrationStatus::NoLegacy;
    QString legacyPath;
    QString newPath;
    QString backupPath;
    QString error;

    //! Only meaningful when status == SkippedNewExists: true when the
    //! legacy file (e.g. next to the executable on Windows) was modified
    //! more recently than the active config. This is the "someone is
    //! editing the wrong file by hand" signal — their edits are being
    //! silently ignored.
    bool legacyModifiedAfterMigration = false;
};

//! Describes what happened to a configuration directory that an older ngPost
//! created under a name derived from the executable. See
//! migrateAppNamedConfigDirIfNeeded().
enum class ConfigDirMigrationStatus
{
    NotNeeded,               //!< no other ngPost configuration folder on this machine
    SkippedTargetConfigured, //!< target already has config/history — nothing was touched
    Migrated,                //!< content adopted; original kept, plus a .save snapshot
    Failed                   //!< the config could not be adopted; see error
};

struct ConfigDirMigrationResult
{
    ConfigDirMigrationStatus status = ConfigDirMigrationStatus::NotNeeded;

    //! The name-derived directory an older ngPost used and that was adopted,
    //! e.g. "~/.config/ngPost-5.4.2-x86_64.AppImage".
    QString legacyDir;

    //! Other ngPost configuration folders found next to it and deliberately
    //! left alone. Only one install can be adopted; the rest are reported so
    //! the user can merge them by hand if they want to.
    QStringList otherLegacyDirs;

    //! configDir() — where settings live from now on.
    QString targetDir;

    //! "<legacyDir>/ngPost.conf.save" (or timestamped variant): a snapshot of
    //! the untouched previous configuration, kept for manual rollback. Empty when the
    //! copy could not be made (non fatal — the copy in targetDir is what
    //! ngPost reads).
    QString savedConfigPath;

    //! Existing SQLite history deliberately retained at its previous path.
    //! The adopted config continues to reference this file, so even a history
    //! of several gigabytes is immediately available without a risky live
    //! SQLite/WAL move or a second copy. Empty when no such default DB exists
    //! or the config already selects another POST_DB.
    QString retainedHistoryPath;

    //! Entry names actually brought over, in the order they were handled.
    QStringList adopted;

    //! Entries that could not be brought over, "<name>: <reason>". The
    //! migration is still reported as Migrated when the configuration file
    //! itself made it: ngPost stays usable and nothing was destroyed.
    QStringList skipped;

    QString error;
};

//! Absolute path to the user's ngPost config directory, without creating it.
//! Use this for read-only defaults built before command-line intent is known.
QString configDirPath();

//! Absolute path to the user's ngPost config directory. Creates it if missing.
//!
//! The path is built from the *generic* config root plus a hard-coded
//! "ngPost", deliberately NOT from QStandardPaths::AppConfigLocation. Those
//! "App" locations append QCoreApplication::applicationName(), which Qt
//! derives from the program file when nobody pins it: the argv[0] file name
//! on Unix, the executable base name on Windows, CFBundleName on macOS. ngPost
//! ships as an AppImage whose file gets renamed on install and on every update
//! (AppImageLauncher), so the config directory used to move on its own —
//! leaving several ngPost.conf and several history databases side by side on
//! the same machine, and CLI posts that never showed up in the GUI history.
//!
//! For the canonical program names (ngPost, ngPost.exe,
//! ngPost.app/Contents/MacOS/ngPost) this returns exactly what the "App"
//! locations returned, so nothing moves for a correctly named install.
QString configDir();

//! The config directory an ngPost identified as `appName` would have used
//! before configDir() was pinned: "<generic config root>/<appName>". Returns
//! an empty string for an empty name. Exposed for the migration and its tests.
QString appNamedConfigDir(const QString &appName);

//! Absolute path to the main configuration file ("ngPost.conf" inside configDir()).
QString configFilePath();

//! Base name of the structured history database inside configDir(), used when
//! ngPost.conf does not set POST_DB. Named here because the config-folder
//! migration recognizes the database and its sidecars and deliberately leaves
//! them untouched.
QString historyDbFileName();

//! Absolute path to the per-user VPN files directory (creates it if missing).
QString vpnDir();

//! Absolute path to the runtime auth-files directory (creates it if missing,
//! chmod 700 if supported). Used for short-lived openvpn `--auth-user-pass`
//! files; nothing persistent lives here.
QString vpnRuntimeDir();

//! Returns the legacy config path used by ngPost 4.16:
//!   - Linux/macOS: "$HOME/.ngPost"
//!   - Windows: "ngPost.conf" next to the executable.
QString legacyConfigFilePath();

//! Returns a non-existing backup path for path: first "path.bak", then
//! "path.YYYYMMDD-HHMMSS.bak" if the simple backup already exists.
QString backupPathFor(const QString &path);

//! If a legacy config file exists and the new config does not, copy the legacy
//! file into the new location and keep the legacy file intact for ngPost 4.16.
//! If both paths resolve to the same file, no write is done unless
//! overwriteConfirmed is true; in that case the file is backed up first.
ConfigMigrationResult migrateLegacyConfigIfNeeded(bool overwriteConfirmed = false);

//! Adopt the settings an older ngPost left in a directory named after the
//! program file, so pinning configDir() never hides a working install.
//!
//! Candidates are found by *marker*, not by name: any sibling directory of
//! configDir() that holds an "ngPost.conf" is an ngPost configuration folder,
//! whatever it is called. Matching the current program name is not enough —
//! an AppImage is typically renamed between versions (ngPost-5.4.AppImage ->
//! ngPost-5.5.AppImage), so at update time the folder to adopt carries the
//! *previous* name. `legacyAppName` (the name Qt derived on its own — read
//! QCoreApplication::applicationName() *before* setApplicationName() pins it)
//! is only used to pick the best candidate when several exist; the most
//! recently configured one wins otherwise.
//!
//! In GUI mode call this before MainWindow reads ngPost_gui.ini. CLI mode may
//! defer it until after syntax/help/version and an explicit -c were handled;
//! normal posting then adopts the folder before the default config is parsed.
//!
//! GUI ini, vpn/ profiles, post-info templates (which resolve relative to the
//! config folder), and future configuration assets are copied. SQLite history
//! is intentionally retained at its old path and referenced by POST_DB.
//!
//! Safety rules, in order:
//!   1. nothing happens if configDir() already holds ngPost.conf, any SQLite
//!      history file, or the one-time stamp: user data is never displaced;
//!   2. a cross-process lock serializes two simultaneous first launches;
//!   3. every non-database entry is copied atomically — templates, GUI ini,
//!      vpn profiles, future assets — merging directories but skipping names
//!      already present, archives, runtime secrets and symbolic links. Copying
//!      rather than moving is
//!      what keeps "-c <old folder>/ngPost.conf" working: a post-info template
//!      resolves relative to the folder of the config file it was named in, so
//!      moving templates away would break scripts that hold that path;
//!   4. the history database and its WAL/SHM files are never copied or moved.
//!      Existing POST_DB values are preserved; if an older hand-written config
//!      has no POST_DB, only its canonical copy is pinned to the old DB path;
//!   5. the source ngPost.conf is copied to ngPost.conf.save and left in
//!      place — renaming it away would invalidate the very path scripts and
//!      cron jobs pass to -c;
//!   6. ngPost.conf is published atomically *last*. If the process stops while
//!      copying assets, the next start sees no configured target and resumes
//!      by filling only missing files;
//!   7. a stamp file records the outcome in configDir(), which keeps the check
//!      one-shot even if the adopted config is later deleted or replaced, and
//!      keeps the "you still have other config folders" notice to a single
//!      appearance.
//!
//! Nothing is deleted or overwritten. The old folder and its database remain
//! usable; the worst case is a partial set of non-conflicting copies in the
//! target plus a report, and a later launch can safely complete it.
//!
//! Runs once per process; the cached result is returned afterwards (it re-runs
//! if called with a different name or after configDir() changed, which only
//! happens in tests). A no-op when the NGPOST_TEST_CONFIG_DIR override is
//! active.
const ConfigDirMigrationResult &migrateAppNamedConfigDirIfNeeded(const QString &legacyAppName);

//! What migrateAppNamedConfigDirIfNeeded() did earlier in this process, for
//! the code that reports it to the user once a log and a GUI exist. Status is
//! NotNeeded when the migration never ran.
const ConfigDirMigrationResult &configDirMigrationResult();
}

#endif // PATHHELPER_H
