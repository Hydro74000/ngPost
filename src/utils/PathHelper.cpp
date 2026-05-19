//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "PathHelper.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace PathHelper
{

namespace
{

#ifdef NGPOST_TESTING
QString testEnvPath(const char *name)
{
    const QByteArray value = qgetenv(name);
    return value.isEmpty() ? QString() : QString::fromLocal8Bit(value);
}
#endif

} // namespace

QString configDir()
{
#ifdef NGPOST_TESTING
    const QString testDir = testEnvPath("NGPOST_TEST_CONFIG_DIR");
    if (!testDir.isEmpty()) {
        QDir().mkpath(testDir);
        return testDir;
    }
#endif

    // QStandardPaths::AppConfigLocation gives us:
    //   Linux   ~/.config/ngPost
    //   macOS   ~/Library/Preferences/<...>/ngPost  (not ideal — see below)
    //   Windows C:/Users/<user>/AppData/Local/ngPost
    // On macOS, Apple guidelines actually recommend "Application Support"
    // (AppLocalDataLocation), which is what most cross-platform Qt apps
    // pick anyway. We use AppConfigLocation on Linux/Windows and
    // AppLocalDataLocation on macOS for the right user-facing path.
#if defined(Q_OS_MAC)
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#else
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
#endif
    QDir().mkpath(d);
    return d;
}

QString configFilePath()
{
    return configDir() + QStringLiteral("/ngPost.conf");
}

QString vpnDir()
{
    QString d = configDir() + QStringLiteral("/vpn");
    QDir().mkpath(d);
    return d;
}

QString vpnRuntimeDir()
{
    // No leading dot in the directory name: confirmed via live-reproduction
    // on a Windows VM that OpenVPNServiceInteractive's impersonation of the
    // calling user refuses to read an auth file from a directory whose name
    // starts with '.'. openvpn.exe then exits with code 1 immediately and
    // the service surfaces "0x20000000 OpenVPN exited with error". Using
    // "runtime" (no dot) works on Windows and is functionally equivalent on
    // Linux — security comes from the per-file permissions we set below,
    // not from the directory being marked hidden.
    QString d = vpnDir() + QStringLiteral("/runtime");
    QDir().mkpath(d);
    // Best-effort: tighten perms so other local users can't list our
    // ephemeral auth files. Qt's setPermissions is portable (no-op where
    // not meaningful).
    QFile::setPermissions(d, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner);
    return d;
}

QString legacyConfigFilePath()
{
#ifdef NGPOST_TESTING
    const QString testHome = testEnvPath("NGPOST_TEST_HOME");
    if (!testHome.isEmpty())
        return testHome + QStringLiteral("/.ngPost");
#endif

    return QDir::homePath() + QStringLiteral("/.ngPost");
}

void migrateLegacyConfigIfNeeded()
{
    QString newPath    = configFilePath();
    QString legacyPath = legacyConfigFilePath();

    if (QFile::exists(newPath))
        return; // already on the new layout

    QFileInfo legacy(legacyPath);
    if (!legacy.exists() || !legacy.isFile())
        return; // no legacy conf to migrate

    // Make sure the parent dir exists (configDir() already does it via mkpath
    // when called above, but be explicit).
    QDir().mkpath(QFileInfo(newPath).absolutePath());

    // Use copy + remove rather than rename so we keep the file readable if
    // something else is mid-write on it. The legacy file is then deleted to
    // avoid double-write on next launch.
    if (QFile::copy(legacyPath, newPath))
        QFile::remove(legacyPath);
}

} // namespace PathHelper
