//========================================================================
//
// tst_WireGuardBackend.cpp — unit tests for the static helpers used by the
// Windows runtime path of WireGuardBackend. Cross-platform on purpose: the
// helpers are pure string/path manipulation, so the same regressions are
// caught whether we run on Linux, macOS or Windows. The actual SCM/Winsock
// interactions are not exercised here (no service registered, no UAC).
//
//========================================================================

#include <QtTest>

#include "vpn/WireGuardBackend.h"

class TestWireGuardBackend : public QObject
{
    Q_OBJECT

private slots:
    void serviceNameFromConfig_strips_directory_and_extension();
    void serviceNameFromConfig_handles_multiple_dots();
    void serviceNameFromConfig_handles_no_extension();
    void serviceNameFromConfig_handles_empty();

    void parseInterfaceAddrAndDns_extracts_address_and_single_dns();
    void parseInterfaceAddrAndDns_extracts_first_dns_when_multiple();
    void parseInterfaceAddrAndDns_clears_dns_when_absent();
    void parseInterfaceAddrAndDns_strips_cidr_from_address();
    void parseInterfaceAddrAndDns_ignores_address_outside_interface_section();
    void parseInterfaceAddrAndDns_returns_false_when_no_address();
    void parseInterfaceAddrAndDns_case_insensitive_keys();
};

void TestWireGuardBackend::serviceNameFromConfig_strips_directory_and_extension()
{
    QCOMPARE(WireGuardBackend::serviceNameFromConfig(QStringLiteral("C:/users/x/vpn/MyProf.conf")),
             QStringLiteral("WireGuardTunnel$MyProf"));
    QCOMPARE(WireGuardBackend::serviceNameFromConfig(QStringLiteral("/tmp/test.conf")),
             QStringLiteral("WireGuardTunnel$test"));
    // Bare filename, no directory.
    QCOMPARE(WireGuardBackend::serviceNameFromConfig(QStringLiteral("home.conf")),
             QStringLiteral("WireGuardTunnel$home"));
}

void TestWireGuardBackend::serviceNameFromConfig_handles_multiple_dots()
{
    // completeBaseName() strips only the LAST extension — "my.prof.conf"
    // becomes "my.prof", which matches what wireguard.exe registers.
    QCOMPARE(WireGuardBackend::serviceNameFromConfig(QStringLiteral("/p/my.prof.conf")),
             QStringLiteral("WireGuardTunnel$my.prof"));
}

void TestWireGuardBackend::serviceNameFromConfig_handles_no_extension()
{
    // No extension at all → completeBaseName() returns the whole filename.
    QCOMPARE(WireGuardBackend::serviceNameFromConfig(QStringLiteral("/p/MyProf")),
             QStringLiteral("WireGuardTunnel$MyProf"));
}

void TestWireGuardBackend::serviceNameFromConfig_handles_empty()
{
    QCOMPARE(WireGuardBackend::serviceNameFromConfig(QString()),
             QStringLiteral("WireGuardTunnel$"));
}

void TestWireGuardBackend::parseInterfaceAddrAndDns_extracts_address_and_single_dns()
{
    QString ip, dns;
    const QString conf = QStringLiteral(
        "[Interface]\n"
        "PrivateKey = 4HRG+OZAxTwGS/UMfPo3+lDqW3l3MJTwl/3oP1S5mEs=\n"
        "Address = 10.42.0.2/32\n"
        "DNS = 1.1.1.1\n"
        "\n"
        "[Peer]\n"
        "PublicKey = abc=\n"
        "Endpoint = vpn.example.com:51820\n"
        "AllowedIPs = 0.0.0.0/0\n");
    QVERIFY(WireGuardBackend::parseInterfaceAddrAndDns(conf, &ip, &dns));
    QCOMPARE(ip,  QStringLiteral("10.42.0.2"));
    QCOMPARE(dns, QStringLiteral("1.1.1.1"));
}

void TestWireGuardBackend::parseInterfaceAddrAndDns_extracts_first_dns_when_multiple()
{
    QString ip, dns;
    const QString conf = QStringLiteral(
        "[Interface]\n"
        "Address = 10.42.0.2/32\n"
        "DNS = 1.1.1.1, 8.8.8.8 ,9.9.9.9\n");
    QVERIFY(WireGuardBackend::parseInterfaceAddrAndDns(conf, &ip, &dns));
    QCOMPARE(ip,  QStringLiteral("10.42.0.2"));
    QCOMPARE(dns, QStringLiteral("1.1.1.1"));
}

void TestWireGuardBackend::parseInterfaceAddrAndDns_clears_dns_when_absent()
{
    QString ip, dns = QStringLiteral("garbage-stays-cleared");
    const QString conf = QStringLiteral(
        "[Interface]\n"
        "Address = 10.0.0.5/24\n"
        "ListenPort = 51820\n");
    QVERIFY(WireGuardBackend::parseInterfaceAddrAndDns(conf, &ip, &dns));
    QCOMPARE(ip,  QStringLiteral("10.0.0.5"));
    QCOMPARE(dns, QString());
}

void TestWireGuardBackend::parseInterfaceAddrAndDns_strips_cidr_from_address()
{
    QString ip, dns;
    // Both /32 and /24 forms must work — we only want the IP.
    const QString conf = QStringLiteral(
        "[Interface]\n"
        "Address = 192.168.1.50/24\n");
    QVERIFY(WireGuardBackend::parseInterfaceAddrAndDns(conf, &ip, &dns));
    QCOMPARE(ip, QStringLiteral("192.168.1.50"));
}

void TestWireGuardBackend::parseInterfaceAddrAndDns_ignores_address_outside_interface_section()
{
    // A stray Address= in [Peer] (or any non-Interface section) must NOT
    // be picked up — that's a different semantic (Peer.AllowedIPs is the
    // closest concept and is named differently anyway).
    QString ip, dns;
    const QString conf = QStringLiteral(
        "[Peer]\n"
        "Address = 10.0.0.99/32\n"
        "AllowedIPs = 10.0.0.99/32\n"
        "\n"
        "[Interface]\n"
        "Address = 10.42.0.7/32\n");
    QVERIFY(WireGuardBackend::parseInterfaceAddrAndDns(conf, &ip, &dns));
    QCOMPARE(ip, QStringLiteral("10.42.0.7"));
}

void TestWireGuardBackend::parseInterfaceAddrAndDns_returns_false_when_no_address()
{
    QString ip = QStringLiteral("untouched-on-failure"), dns;
    const QString conf = QStringLiteral(
        "[Interface]\n"
        "PrivateKey = 4HRG+OZAxTwGS/UMfPo3+lDqW3l3MJTwl/3oP1S5mEs=\n"
        "ListenPort = 51820\n");
    QVERIFY(!WireGuardBackend::parseInterfaceAddrAndDns(conf, &ip, &dns));
}

void TestWireGuardBackend::parseInterfaceAddrAndDns_case_insensitive_keys()
{
    // WireGuard's ini parser treats keys case-insensitively (sort of —
    // wg-quick is lenient). Our regex uses (?i) so "address" / "DNS" both
    // match.
    QString ip, dns;
    const QString conf = QStringLiteral(
        "[Interface]\n"
        "address = 10.1.2.3/32\n"
        "dns = 1.1.1.1\n");
    QVERIFY(WireGuardBackend::parseInterfaceAddrAndDns(conf, &ip, &dns));
    QCOMPARE(ip,  QStringLiteral("10.1.2.3"));
    QCOMPARE(dns, QStringLiteral("1.1.1.1"));
}

QTEST_APPLESS_MAIN(TestWireGuardBackend)
#include "tst_WireGuardBackend.moc"
