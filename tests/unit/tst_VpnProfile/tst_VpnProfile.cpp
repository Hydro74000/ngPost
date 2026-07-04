// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_VpnProfile.cpp — VpnProfile struct + VpnManager::Backend enum helpers.
//
//========================================================================

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>

#include "nntp/NntpServerParams.h"
#include "TestEnv.h"
#include "utils/PathHelper.h"
#include "vpn/VpnManager.h"
#include "vpn/VpnProfile.h"

using ngpost::tests::HomeSandbox;

namespace {
struct TestVpnHelperFile
{
    QString path;
    bool created;

    TestVpnHelperFile()
        : path(), created(false)
    {
        QDir appDir(QCoreApplication::applicationDirPath());
        appDir.mkpath(QStringLiteral("vpn/scripts"));
        path = appDir.filePath(QStringLiteral("vpn/scripts/ngpost-vpn-helper.sh"));
        if (QFileInfo::exists(path))
            return;

        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write("#!/bin/sh\nexit 0\n");
            f.close();
            QFile::setPermissions(path,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                QFileDevice::ReadOther | QFileDevice::ExeOther);
            created = true;
        }
    }

    ~TestVpnHelperFile()
    {
        if (created)
            QFile::remove(path);
    }
};
}

class TestVpnProfile : public QObject
{
    Q_OBJECT

private slots:
    //! Default-constructed profile is invalid (name + configFileName empty).
    void default_profile_is_invalid();

    //! A profile is "valid" iff both `name` and `configFileName` are non-empty.
    void isValid_requires_name_and_config_file();

    //! absoluteConfigPath() is "<vpnDir>/<configFileName>". Empty configFile →
    //! empty result (guarded path).
    void absoluteConfigPath_under_vpnDir();
    void absoluteConfigPath_empty_when_configFile_empty();

    //! backendToString() ↔ backendFromString() round-trip for both backends.
    void backend_roundtrip();

    //! backendFromString() accepts both the canonical name AND a short alias,
    //! is case-insensitive, and tolerates surrounding whitespace.
    void backendFromString_accepts_aliases();
    void backendFromString_is_case_insensitive();
    void backendFromString_trims_whitespace();

    //! Unknown backend strings set ok=false and fall back to OpenVPN.
    void backendFromString_unknown_sets_ok_false();

    //! The global VPN_AUTO_CONNECT switch is ignored when no active VPN profile
    //! is selected/configured.
    void master_switch_ignored_without_active_vpn_profile();

    //! Per-server Use VPN remains fail-closed and logs clear guidance when VPN
    //! setup is incomplete.
    void per_server_useVpn_blocks_and_logs_guidance_when_vpn_incomplete();
};

void TestVpnProfile::default_profile_is_invalid()
{
    VpnProfile p;
    QCOMPARE(p.name, QString());
    QCOMPARE(p.configFileName, QString());
    QCOMPARE(p.hasAuth, false);
    QCOMPARE(p.backend, VpnManager::Backend::OpenVPN);
    QVERIFY(!p.isValid());
}

void TestVpnProfile::isValid_requires_name_and_config_file()
{
    VpnProfile p;
    p.name = "MyProf";
    QVERIFY2(!p.isValid(), "profile with name but no configFileName must be invalid");

    p.name = "";
    p.configFileName = "myprof.ovpn";
    QVERIFY2(!p.isValid(), "profile with configFileName but no name must be invalid");

    p.name = "MyProf";
    QVERIFY(p.isValid());
}

void TestVpnProfile::absoluteConfigPath_under_vpnDir()
{
    HomeSandbox sandbox;

    VpnProfile p;
    p.name = "MyProf";
    p.configFileName = "myprof.ovpn";

    const QString abs = p.absoluteConfigPath();
    QCOMPARE(abs, PathHelper::vpnDir() + QStringLiteral("/myprof.ovpn"));
    QVERIFY(abs.startsWith(sandbox.rootPath()));
}

void TestVpnProfile::absoluteConfigPath_empty_when_configFile_empty()
{
    HomeSandbox sandbox;

    VpnProfile p;
    p.name = "Anon";

    QCOMPARE(p.absoluteConfigPath(), QString());
}

void TestVpnProfile::backend_roundtrip()
{
    using Backend = VpnManager::Backend;

    for (Backend b : { Backend::OpenVPN, Backend::WireGuard }) {
        bool ok = false;
        const QString s = VpnManager::backendToString(b);
        const Backend back = VpnManager::backendFromString(s, &ok);
        QVERIFY2(ok, qPrintable(QStringLiteral("backendFromString failed for canonical name '%1'").arg(s)));
        QCOMPARE(back, b);
    }
}

void TestVpnProfile::backendFromString_accepts_aliases()
{
    bool ok = false;

    QCOMPARE(VpnManager::backendFromString("openvpn", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);

    QCOMPARE(VpnManager::backendFromString("ovpn", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);

    QCOMPARE(VpnManager::backendFromString("wireguard", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);

    QCOMPARE(VpnManager::backendFromString("wg", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
}

void TestVpnProfile::backendFromString_is_case_insensitive()
{
    bool ok = false;
    QCOMPARE(VpnManager::backendFromString("OpenVPN", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);
    QCOMPARE(VpnManager::backendFromString("WIREGUARD", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
    QCOMPARE(VpnManager::backendFromString("WG", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
}

void TestVpnProfile::backendFromString_trims_whitespace()
{
    bool ok = false;
    QCOMPARE(VpnManager::backendFromString("  openvpn  ", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);
    QCOMPARE(VpnManager::backendFromString("\twireguard\n", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
}

void TestVpnProfile::backendFromString_unknown_sets_ok_false()
{
    bool ok = true;
    const auto fallback = VpnManager::backendFromString("noway", &ok);
    QVERIFY2(!ok, "ok should be false for unknown backend string");
    QCOMPARE(fallback, VpnManager::Backend::OpenVPN); // documented default

    ok = true;
    VpnManager::backendFromString(QString(), &ok);
    QVERIFY(!ok);
}

void TestVpnProfile::master_switch_ignored_without_active_vpn_profile()
{
    HomeSandbox sandbox;
    TestVpnHelperFile helper;

    VpnManager manager;
    manager.setAutoConnect(true);
    if (!manager.vpnFeatureAvailable())
        QSKIP("No VPN helper/prerequisite available on this platform");

    QVERIFY2(!manager.forceAllConnectionsThroughVpn(),
             "VPN_AUTO_CONNECT must be neutral when no active profile is selected");

    NntpServerParams server(QStringLiteral("news.example.org"));
    QList<NntpServerParams *> servers;
    servers << &server;

    QVERIFY(!manager.jobNeedsVpn(servers));
    QCOMPARE(manager.admitJob(servers), VpnManager::Admission::Proceed);
}

void TestVpnProfile::per_server_useVpn_blocks_and_logs_guidance_when_vpn_incomplete()
{
    HomeSandbox sandbox;
    VpnManager manager;

    NntpServerParams server(QStringLiteral("news.example.org"));
    server.useVpn = true;
    QList<NntpServerParams *> servers;
    servers << &server;

    int unavailableCount = 0;
    QString unavailableDetail;
    QObject::connect(&manager, &VpnManager::vpnRequiredButUnavailable,
                     &manager,
                     [&](VpnManager::JobBlockReason, QString const &detail) {
        ++unavailableCount;
        unavailableDetail = detail;
    });

    int statusCount = 0;
    QString statusLine;
    QObject::connect(&manager, &VpnManager::statusLine,
                     &manager,
                     [&](QString const &line) {
        ++statusCount;
        statusLine = line;
    });

    QCOMPARE(manager.admitJob(servers), VpnManager::Admission::Blocked);
    QCOMPARE(unavailableCount, 1);
    QVERIFY2(statusCount > 0, "per-server Use VPN refusal must be written to the post log");

    QVERIFY(unavailableDetail.contains(QStringLiteral("Use VPN is enabled")));
    QVERIFY(unavailableDetail.contains(QStringLiteral("news.example.org")));
    QVERIFY(unavailableDetail.contains(QStringLiteral("VPN is not correctly configured")));
    QVERIFY(unavailableDetail.contains(QStringLiteral("VPN button")));
    QVERIFY(unavailableDetail.contains(QStringLiteral("Use VPN checkbox")));
    QCOMPARE(statusLine, unavailableDetail);
}

QTEST_APPLESS_MAIN(TestVpnProfile)
#include "tst_VpnProfile.moc"
