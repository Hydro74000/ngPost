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

//! Cross-platform paths for ngPost configuration and runtime data.
//!
//! Resolution:
//!  - Linux  : $XDG_CONFIG_HOME/ngPost/   (fallback ~/.config/ngPost/)
//!  - Windows: %APPDATA%/ngPost/
//!  - macOS  : ~/Library/Application Support/ngPost/
//!
//! Layout:
//!  <configDir>/
//!     ngPost.conf      main configuration (was ~/.ngPost on Linux/macOS
//!                      and ngPost.conf next to the executable on Windows)
//!     vpn/             per-user VPN profile files (.ovpn / .conf)
//!     vpn/runtime/     short-lived auth files (chmod 600) — only while a
//!                      VPN connection is active
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
};

//! Absolute path to the user's ngPost config directory. Creates it if missing.
QString configDir();

//! Absolute path to the main configuration file ("ngPost.conf" inside configDir()).
QString configFilePath();

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
}

#endif // PATHHELPER_H
