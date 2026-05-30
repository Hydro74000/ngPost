//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "PathHelper.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
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

bool samePath(const QString &a, const QString &b)
{
    QFileInfo ai(a);
    QFileInfo bi(b);
    const QString ac = ai.canonicalFilePath();
    const QString bc = bi.canonicalFilePath();
    if (!ac.isEmpty() && !bc.isEmpty())
        return ac == bc;

    return QDir::cleanPath(ai.absoluteFilePath())
           == QDir::cleanPath(bi.absoluteFilePath());
}

ConfigMigrationResult result(ConfigMigrationStatus status,
                             const QString &legacyPath,
                             const QString &newPath,
                             const QString &backupPath = QString(),
                             const QString &error = QString())
{
    ConfigMigrationResult r;
    r.status     = status;
    r.legacyPath = legacyPath;
    r.newPath    = newPath;
    r.backupPath = backupPath;
    r.error      = error;
    return r;
}

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
    const QString testLegacy = testEnvPath("NGPOST_TEST_LEGACY_CONFIG");
    if (!testLegacy.isEmpty())
        return testLegacy;
#endif

#if defined(Q_OS_WIN) || defined(WIN32) || defined(__MINGW64__)
#ifdef NGPOST_TESTING
    const QString testHome = testEnvPath("NGPOST_TEST_HOME");
    if (!testHome.isEmpty())
        return testHome + QStringLiteral("/ngPost.conf");
#endif
    return QCoreApplication::applicationDirPath() + QStringLiteral("/ngPost.conf");
#else
#ifdef NGPOST_TESTING
    const QString testHome = testEnvPath("NGPOST_TEST_HOME");
    if (!testHome.isEmpty())
        return testHome + QStringLiteral("/.ngPost");
#endif
    return QDir::homePath() + QStringLiteral("/.ngPost");
#endif
}

QString backupPathFor(const QString &path)
{
    const QString simple = path + QStringLiteral(".bak");
    if (!QFile::exists(simple))
        return simple;

    const QFileInfo fi(path);
    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString candidate = QStringLiteral("%1/%2.%3.bak")
                            .arg(fi.absolutePath(), fi.fileName(), stamp);
    if (!QFile::exists(candidate))
        return candidate;

    for (int i = 1; ; ++i) {
        candidate = QStringLiteral("%1/%2.%3.%4.bak")
                        .arg(fi.absolutePath(), fi.fileName(), stamp)
                        .arg(i);
        if (!QFile::exists(candidate))
            return candidate;
    }
}

ConfigMigrationResult migrateLegacyConfigIfNeeded(bool overwriteConfirmed)
{
    QString newPath    = configFilePath();
    QString legacyPath = legacyConfigFilePath();

    QFileInfo legacy(legacyPath);
    if (!legacy.exists() || !legacy.isFile()) {
        if (QFile::exists(newPath))
            return result(ConfigMigrationStatus::AlreadyMigrated, legacyPath, newPath);
        return result(ConfigMigrationStatus::NoLegacy, legacyPath, newPath);
    }

    if (samePath(legacyPath, newPath)) {
        if (!overwriteConfirmed)
            return result(ConfigMigrationStatus::NeedsOverwriteConfirmation,
                          legacyPath,
                          newPath);

        const QString backupPath = backupPathFor(newPath);
        if (!QFile::copy(newPath, backupPath))
            return result(ConfigMigrationStatus::BackupFailed,
                          legacyPath,
                          newPath,
                          backupPath,
                          QStringLiteral("could not create backup"));

        return result(ConfigMigrationStatus::AlreadyMigrated,
                      legacyPath,
                      newPath,
                      backupPath);
    }

    if (QFile::exists(newPath))
        return result(ConfigMigrationStatus::SkippedNewExists,
                      legacyPath,
                      newPath);

    // Make sure the parent dir exists (configDir() already does it via mkpath
    // when called above, but be explicit).
    QDir().mkpath(QFileInfo(newPath).absolutePath());

    if (!QFile::copy(legacyPath, newPath))
        return result(ConfigMigrationStatus::CopyFailed,
                      legacyPath,
                      newPath,
                      QString(),
                      QStringLiteral("could not copy legacy config"));

    return result(ConfigMigrationStatus::CopiedAndKeptLegacy,
                  legacyPath,
                  newPath);
}

} // namespace PathHelper
