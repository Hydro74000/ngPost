// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_PathHelper.cpp — XDG / appdata directory resolution, vpn dir layout,
// non-destructive legacy config migration.
//
//========================================================================

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "TestEnv.h"
#include "utils/PathHelper.h"

using ngpost::tests::HomeSandbox;

class TestPathHelper : public QObject
{
    Q_OBJECT

private slots:
    //! configDir() must live under the sandboxed HOME and exist after the call.
    void configDir_under_sandbox_and_created();

    //! configFilePath() ends with ngPost.conf and is a child of configDir().
    void configFilePath_inside_configDir();

    //! vpnDir() is "<configDir>/vpn" and is created on demand.
    void vpnDir_subdir_of_configDir_and_created();

    //! vpnRuntimeDir() must NOT have a leading dot in its final segment
    //! (regression guard for the Windows openvpn breakage fixed in 31c4a8c).
    void vpnRuntimeDir_no_leading_dot_in_basename();

    //! migrateLegacyConfigIfNeeded() copies the legacy config into the modern
    //! location and keeps the legacy file for ngPost 4.16.
    void migrateLegacyConfigIfNeeded_copies_and_keeps_legacy();

    //! Called twice in a row: the second call must be a no-op (no exception,
    //! no overwrite of the migrated file).
    void migrateLegacyConfigIfNeeded_is_idempotent();

    //! With no legacy file and no new file present, the migrate call should
    //! create nothing — it must not invent an empty ngPost.conf.
    void migrateLegacyConfigIfNeeded_no_legacy_no_op();

    //! If the new config already exists, the legacy file (if any) is left
    //! alone — we do not overwrite a configured user.
    void migrateLegacyConfigIfNeeded_keeps_new_when_both_present();

    //! backupPathFor() uses .bak first, then an available timestamped path.
    void backupPathFor_prefers_simple_then_timestamped();

    //! If source and destination resolve to the same path, migration requires
    //! explicit confirmation and leaves the file untouched otherwise.
    void migrateLegacyConfigIfNeeded_same_path_requires_confirmation();

    //! A confirmed same-path migration creates a backup before continuing.
    void migrateLegacyConfigIfNeeded_same_path_confirmed_creates_backup();

    //! Test override lets us simulate the Windows legacy ngPost.conf path.
    void legacyConfigFilePath_honors_test_override();
};

void TestPathHelper::configDir_under_sandbox_and_created()
{
    HomeSandbox sandbox;
    const QString d = PathHelper::configDir();

    QVERIFY(QFileInfo(d).exists());
    QVERIFY(QFileInfo(d).isDir());
    QVERIFY2(d.startsWith(sandbox.rootPath()),
             qPrintable(QStringLiteral("configDir not under sandbox: %1").arg(d)));
}

void TestPathHelper::configFilePath_inside_configDir()
{
    HomeSandbox sandbox;
    const QString f = PathHelper::configFilePath();
    QVERIFY(f.endsWith(QStringLiteral("/ngPost.conf")));
    QCOMPARE(QFileInfo(f).absolutePath(), PathHelper::configDir());
}

void TestPathHelper::vpnDir_subdir_of_configDir_and_created()
{
    HomeSandbox sandbox;
    const QString v = PathHelper::vpnDir();

    QVERIFY(QFileInfo(v).exists());
    QVERIFY(QFileInfo(v).isDir());
    QCOMPARE(QFileInfo(v).absolutePath(), PathHelper::configDir());
    QCOMPARE(QFileInfo(v).fileName(), QStringLiteral("vpn"));
}

void TestPathHelper::vpnRuntimeDir_no_leading_dot_in_basename()
{
    HomeSandbox sandbox;
    const QString r = PathHelper::vpnRuntimeDir();
    const QString basename = QFileInfo(r).fileName();

    QVERIFY2(!basename.startsWith('.'),
             qPrintable(QStringLiteral(
                 "vpnRuntimeDir basename starts with '.', breaks OpenVPNServiceInteractive on Windows. Got: %1")
                            .arg(basename)));
    QCOMPARE(basename, QStringLiteral("runtime"));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_copies_and_keeps_legacy()
{
    HomeSandbox sandbox;
    const QString legacy = PathHelper::legacyConfigFilePath();

    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("lang = EN\nhost = legacy.example.com\n");
    f.close();
    QVERIFY(QFile::exists(legacy));

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::CopiedAndKeptLegacy);
    QCOMPARE(r.legacyPath, legacy);

    const QString newConf = PathHelper::configFilePath();
    QVERIFY2(QFile::exists(newConf), "new config not created");
    QVERIFY2(QFile::exists(legacy), "legacy config should be kept for ngPost 4.16");

    QFile g(newConf);
    QVERIFY(g.open(QIODevice::ReadOnly));
    const QByteArray content = g.readAll();
    QVERIFY(content.contains("host = legacy.example.com"));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_is_idempotent()
{
    HomeSandbox sandbox;
    const QString legacy = PathHelper::legacyConfigFilePath();
    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("lang = FR\n");
    f.close();

    PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::CopiedAndKeptLegacy);

    const QString newConf = PathHelper::configFilePath();
    QVERIFY(QFile::exists(newConf));
    const qint64 sizeAfterFirst = QFileInfo(newConf).size();

    // Second call should not modify anything.
    r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::SkippedNewExists);

    QVERIFY(QFile::exists(newConf));
    QCOMPARE(QFileInfo(newConf).size(), sizeAfterFirst);
    QVERIFY(QFile::exists(legacy));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_no_legacy_no_op()
{
    HomeSandbox sandbox;
    const QString newConf = PathHelper::configFilePath();
    const QString legacy  = PathHelper::legacyConfigFilePath();

    QVERIFY(!QFile::exists(legacy));
    QVERIFY(!QFile::exists(newConf));

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::NoLegacy);

    QVERIFY2(!QFile::exists(newConf),
             "migrate created an empty config when no legacy existed");
}

void TestPathHelper::migrateLegacyConfigIfNeeded_keeps_new_when_both_present()
{
    HomeSandbox sandbox;
    const QString newConf = PathHelper::configFilePath();
    const QString legacy  = PathHelper::legacyConfigFilePath();

    QDir().mkpath(QFileInfo(newConf).absolutePath());
    {
        QFile n(newConf);
        QVERIFY(n.open(QIODevice::WriteOnly));
        n.write("from_new = true\n");
    }
    {
        QFile l(legacy);
        QVERIFY(l.open(QIODevice::WriteOnly));
        l.write("from_legacy = true\n");
    }

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::SkippedNewExists);

    QFile n(newConf);
    QVERIFY(n.open(QIODevice::ReadOnly));
    const QByteArray content = n.readAll();
    QVERIFY2(content.contains("from_new"),
             "new config was overwritten by the migration");
    QVERIFY2(!content.contains("from_legacy"),
             "legacy content leaked into pre-existing new config");
    QVERIFY2(QFile::exists(legacy), "legacy config should be kept");
}

void TestPathHelper::backupPathFor_prefers_simple_then_timestamped()
{
    HomeSandbox sandbox;
    const QString conf = PathHelper::configFilePath();
    QDir().mkpath(QFileInfo(conf).absolutePath());

    QCOMPARE(PathHelper::backupPathFor(conf), conf + QStringLiteral(".bak"));

    QFile bak(conf + QStringLiteral(".bak"));
    QVERIFY(bak.open(QIODevice::WriteOnly));
    bak.write("existing backup\n");
    bak.close();

    const QString timestamped = PathHelper::backupPathFor(conf);
    QVERIFY(timestamped != conf + QStringLiteral(".bak"));
    QVERIFY(timestamped.endsWith(QStringLiteral(".bak")));
    QVERIFY(timestamped.contains(QFileInfo(conf).fileName() + QStringLiteral(".")));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_same_path_requires_confirmation()
{
    HomeSandbox sandbox;
    const QString conf = PathHelper::configFilePath();
    qputenv("NGPOST_TEST_LEGACY_CONFIG", conf.toLocal8Bit());

    QDir().mkpath(QFileInfo(conf).absolutePath());
    QFile f(conf);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("same_path = true\n");
    f.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::NeedsOverwriteConfirmation);
    QVERIFY(QFile::exists(conf));
    QVERIFY(!QFile::exists(conf + QStringLiteral(".bak")));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_same_path_confirmed_creates_backup()
{
    HomeSandbox sandbox;
    const QString conf = PathHelper::configFilePath();
    qputenv("NGPOST_TEST_LEGACY_CONFIG", conf.toLocal8Bit());

    QDir().mkpath(QFileInfo(conf).absolutePath());
    QFile f(conf);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("same_path = true\n");
    f.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded(true);
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::AlreadyMigrated);
    QVERIFY(!r.backupPath.isEmpty());
    QVERIFY(QFile::exists(r.backupPath));

    QFile backup(r.backupPath);
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QVERIFY(backup.readAll().contains("same_path = true"));
}

void TestPathHelper::legacyConfigFilePath_honors_test_override()
{
    HomeSandbox sandbox;
    const QString injected = sandbox.rootPath() + QStringLiteral("/app/ngPost.conf");
    qputenv("NGPOST_TEST_LEGACY_CONFIG", injected.toLocal8Bit());

    QCOMPARE(PathHelper::legacyConfigFilePath(), injected);

    QDir().mkpath(QFileInfo(injected).absolutePath());
    QFile legacy(injected);
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    legacy.write("windows_legacy = true\n");
    legacy.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::CopiedAndKeptLegacy);
    QVERIFY(QFile::exists(injected));
    QVERIFY(QFile::exists(PathHelper::configFilePath()));
}

QTEST_APPLESS_MAIN(TestPathHelper)
#include "tst_PathHelper.moc"
