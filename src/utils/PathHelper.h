//========================================================================
//
// Copyright (C) 2026 ngPost contributors
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
//!  - Windows: %APPDATA%\ngPost\
//!  - macOS  : ~/Library/Application Support/ngPost/
//!
//! Layout:
//!  <configDir>/
//!     ngPost.conf      main configuration (was ~/.ngPost on Linux)
//!     vpn/             per-user VPN profile files (.ovpn / .conf)
//!     vpn/.runtime/    short-lived auth files (chmod 600) — only while a
//!                      VPN connection is active
namespace PathHelper
{
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

//! Returns the legacy path "$HOME/.ngPost" (Linux) / equivalent. Used only by
//! migrateLegacyConfigIfNeeded.
QString legacyConfigFilePath();

//! If the legacy config file exists and the new XDG one does not, move the
//! legacy file into place. Idempotent. Should be called once at startup,
//! BEFORE parseDefaultConfig.
void    migrateLegacyConfigIfNeeded();
}

#endif // PATHHELPER_H
