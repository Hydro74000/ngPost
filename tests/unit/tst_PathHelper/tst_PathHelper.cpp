//========================================================================
//
// tst_PathHelper.cpp — XDG / appdata directory resolution, vpn dir layout,
// legacy ~/.ngPost migration.
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

    //! migrateLegacyConfigIfNeeded() copies ~/.ngPost into the XDG location
    //! and removes the legacy file. New config wins if both exist.
    void migrateLegacyConfigIfNeeded_moves_and_keeps_content();

    //! Called twice in a row: the second call must be a no-op (no exception,
    //! no overwrite of the migrated file).
    void migrateLegacyConfigIfNeeded_is_idempotent();

    //! With no legacy file and no new file present, the migrate call should
    //! create nothing — it must not invent an empty ngPost.conf.
    void migrateLegacyConfigIfNeeded_no_legacy_no_op();

    //! If the new config already exists, the legacy file (if any) is left
    //! alone — we do not overwrite a configured user.
    void migrateLegacyConfigIfNeeded_keeps_new_when_both_present();
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

void TestPathHelper::migrateLegacyConfigIfNeeded_moves_and_keeps_content()
{
    HomeSandbox sandbox;
    const QString legacy = PathHelper::legacyConfigFilePath();

    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("lang = EN\nhost = legacy.example.com\n");
    f.close();
    QVERIFY(QFile::exists(legacy));

    PathHelper::migrateLegacyConfigIfNeeded();

    const QString newConf = PathHelper::configFilePath();
    QVERIFY2(QFile::exists(newConf), "new config not created");
    QVERIFY2(!QFile::exists(legacy), "legacy config not removed");

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

    PathHelper::migrateLegacyConfigIfNeeded();

    const QString newConf = PathHelper::configFilePath();
    QVERIFY(QFile::exists(newConf));
    const qint64 sizeAfterFirst = QFileInfo(newConf).size();

    // Second call should not modify anything.
    PathHelper::migrateLegacyConfigIfNeeded();

    QVERIFY(QFile::exists(newConf));
    QCOMPARE(QFileInfo(newConf).size(), sizeAfterFirst);
    QVERIFY(!QFile::exists(legacy));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_no_legacy_no_op()
{
    HomeSandbox sandbox;
    const QString newConf = PathHelper::configFilePath();
    const QString legacy  = PathHelper::legacyConfigFilePath();

    QVERIFY(!QFile::exists(legacy));
    QVERIFY(!QFile::exists(newConf));

    PathHelper::migrateLegacyConfigIfNeeded();

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

    PathHelper::migrateLegacyConfigIfNeeded();

    QFile n(newConf);
    QVERIFY(n.open(QIODevice::ReadOnly));
    const QByteArray content = n.readAll();
    QVERIFY2(content.contains("from_new"),
             "new config was overwritten by the migration");
    QVERIFY2(!content.contains("from_legacy"),
             "legacy content leaked into pre-existing new config");
}

QTEST_APPLESS_MAIN(TestPathHelper)
#include "tst_PathHelper.moc"
