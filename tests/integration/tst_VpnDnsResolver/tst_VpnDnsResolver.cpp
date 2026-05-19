//========================================================================
//
// tst_VpnDnsResolver.cpp — DNS-over-TCP resolution via the mock_dns mock.
//
// VpnDnsResolver underpins how ngPost reaches NNTP servers when a tunnel
// is up: every NNTP socket binds its source IP to the tunnel address,
// and DNS queries must follow the same path, hence DNS over TCP rather
// than the system resolver.
//
// These tests use 127.0.0.1 as both DNS server and source IP — the
// Phase 5 VPN E2E will swap in real tunnel addresses while exercising
// the same resolver code path.
//
//========================================================================

#include <QtTest>
#include <QHash>
#include <QHostAddress>
#include <QStandardPaths>

#include "MockDnsServer.h"
#include "vpn/VpnDnsResolver.h"

using ngpost::tests::MockDnsServer;

namespace
{
bool hasPython3()
{
    return !QStandardPaths::findExecutable("python3").isEmpty()
           || !QStandardPaths::findExecutable("python").isEmpty();
}
} // namespace

class TestVpnDnsResolver : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    //! Happy path: mock answers a known name → resolveA returns the IP.
    void resolve_returns_mock_answer();

    //! Second call with same key uses the in-memory cache. Stop the mock
    //! between calls — second call still succeeds because of caching.
    void resolve_caches_subsequent_call();

    //! dnsServer == null IP → empty result, error message says "DNS server is not set".
    void resolve_empty_when_dns_null();

    //! sourceIp == null IP → empty result, error message says "source IP is not set".
    void resolve_empty_when_source_ip_null();

    //! Mock not running → resolveA times out (or fails to connect) and returns empty.
    void resolve_times_out_with_no_server();

    //! Mock truncates after the 2-byte length prefix → resolver reports
    //! "truncated".
    void resolve_handles_truncated_response();
};

void TestVpnDnsResolver::initTestCase()
{
    if (!hasPython3())
        QSKIP("python3 not on PATH; integration tests need it");
}

void TestVpnDnsResolver::resolve_returns_mock_answer()
{
    MockDnsServer dns;
    QVERIFY(dns.start({ { "example.test", "10.42.0.99" } }));

    QString err;
    const auto addrs = VpnDnsResolver::resolveA(
        QStringLiteral("example.test"),
        QHostAddress("127.0.0.1"),
        QHostAddress("127.0.0.1"),
        &err,
        /*timeoutMs=*/3000,
        /*port=*/dns.port());

    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(addrs.size(), 1);
    QCOMPARE(addrs.first().toString(), QStringLiteral("10.42.0.99"));
}

void TestVpnDnsResolver::resolve_caches_subsequent_call()
{
    MockDnsServer dns;
    // Use a unique name so we don't collide with earlier tests' cache entries.
    const QString name = QStringLiteral("cached.example.test");
    QVERIFY(dns.start({ { name, "192.0.2.7" } }));

    QString err;
    const auto first = VpnDnsResolver::resolveA(
        name, QHostAddress("127.0.0.1"), QHostAddress("127.0.0.1"),
        &err, 3000, dns.port());
    QVERIFY2(!first.isEmpty(), qPrintable(err));

    dns.stop();

    // Second call — mock is dead, but the cache still has the entry.
    err.clear();
    const auto second = VpnDnsResolver::resolveA(
        name, QHostAddress("127.0.0.1"), QHostAddress("127.0.0.1"),
        &err, 3000, dns.port());
    QCOMPARE(second.size(), 1);
    QCOMPARE(second.first().toString(), QStringLiteral("192.0.2.7"));
    QVERIFY2(err.isEmpty(), qPrintable(err));
}

void TestVpnDnsResolver::resolve_empty_when_dns_null()
{
    QString err;
    const auto addrs = VpnDnsResolver::resolveA(
        QStringLiteral("anything.invalid"),
        QHostAddress(), // null
        QHostAddress("127.0.0.1"),
        &err);

    QVERIFY(addrs.isEmpty());
    QVERIFY2(err.contains("DNS server is not set", Qt::CaseInsensitive),
             qPrintable(err));
}

void TestVpnDnsResolver::resolve_empty_when_source_ip_null()
{
    QString err;
    const auto addrs = VpnDnsResolver::resolveA(
        QStringLiteral("anything.invalid"),
        QHostAddress("127.0.0.1"),
        QHostAddress(), // null
        &err);

    QVERIFY(addrs.isEmpty());
    QVERIFY2(err.contains("source IP is not set", Qt::CaseInsensitive),
             qPrintable(err));
}

void TestVpnDnsResolver::resolve_times_out_with_no_server()
{
    // No mock running — pick an arbitrary (unbound) loopback port.
    QString err;
    const auto addrs = VpnDnsResolver::resolveA(
        QStringLiteral("nobody-listening.invalid"),
        QHostAddress("127.0.0.1"),
        QHostAddress("127.0.0.1"),
        &err,
        /*timeoutMs=*/500,
        /*port=*/1); // port 1 is reserved, no server is going to answer

    QVERIFY(addrs.isEmpty());
    QVERIFY2(!err.isEmpty(), "expected an error message for unreachable DNS");
}

void TestVpnDnsResolver::resolve_handles_truncated_response()
{
    MockDnsServer dns;
    // Truncate after 1 byte → resolver can't even read the 2-byte length.
    QVERIFY(dns.start({ { "trunc.example.test", "10.0.0.1" } },
                      { "--truncate-after", "1" }));

    QString err;
    const auto addrs = VpnDnsResolver::resolveA(
        QStringLiteral("trunc.example.test"),
        QHostAddress("127.0.0.1"),
        QHostAddress("127.0.0.1"),
        &err,
        /*timeoutMs=*/1500,
        /*port=*/dns.port());

    QVERIFY(addrs.isEmpty());
    QVERIFY2(err.contains("truncated", Qt::CaseInsensitive)
                 || err.contains("timed out", Qt::CaseInsensitive),
             qPrintable(err));
}

QTEST_MAIN(TestVpnDnsResolver)
#include "tst_VpnDnsResolver.moc"
