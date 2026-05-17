//========================================================================
//
// tst_VpnProfile.cpp — VpnProfile struct + VpnManager::Backend enum helpers.
//
//========================================================================

#include <QtTest>
#include <QDir>

#include "TestEnv.h"
#include "utils/PathHelper.h"
#include "vpn/VpnManager.h"
#include "vpn/VpnProfile.h"

using ngpost::tests::HomeSandbox;

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

QTEST_APPLESS_MAIN(TestVpnProfile)
#include "tst_VpnProfile.moc"
