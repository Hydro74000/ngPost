//========================================================================
//
// tst_WindowsBindHelper.cpp — Windows-only Winsock readiness/bind helpers.
//
// The helper exists to work around a Windows-specific timing window where
// a freshly-created tunnel address is visible to Qt (QNetworkInterface)
// before Winsock accepts bind() on it. We exercise it here on loopback
// (always available) to catch regressions in:
//   - input validation (null/IPv6 rejection)
//   - the raw-socket bind() path
//   - setSocketDescriptor handover into QAbstractSocket
//
// On non-Windows the helper isn't compiled, so the entire suite is a
// single QSKIP. Keeping the test enrolled on every platform keeps the
// runner output consistent and surfaces accidental Linux/macOS breaks
// (rare, since the file is fully Q_OS_WIN-guarded — but cheap insurance).
//
//========================================================================

#include <QtTest>

#ifdef Q_OS_WIN
#include <QHostAddress>
#include <QTcpSocket>
#include "vpn/WindowsBindHelper.h"
#endif

class TestWindowsBindHelper : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

#ifdef Q_OS_WIN
    void canBindLocalAddress_succeeds_on_loopback();
    void canBindLocalAddress_rejects_null_address();
    void canBindLocalAddress_rejects_ipv6();
    void canBindLocalAddress_rejects_unbound_address();
    void bindAndAttach_loopback_socket_reaches_bound_state();
    void bindAndAttach_rejects_null_socket();
    void bindAndAttach_rejects_null_address();
#endif
};

void TestWindowsBindHelper::initTestCase()
{
#ifndef Q_OS_WIN
    QSKIP("WindowsBindHelper is Windows-only (Winsock/IP_UNICAST_IF). "
          "On Linux/macOS the same source-binding role is filled by "
          "SO_BINDTODEVICE / IP_BOUND_IF, exercised by tst_VpnE2E_*.");
#endif
}

#ifdef Q_OS_WIN

void TestWindowsBindHelper::canBindLocalAddress_succeeds_on_loopback()
{
    // 127.0.0.1 is always present on a healthy Windows runner. If even this
    // bind fails, something is fundamentally broken in the runner image and
    // we want to know about it loudly.
    QString err;
    QVERIFY2(WindowsBindHelper::canBindLocalAddress(QHostAddress::LocalHost, &err),
             qPrintable(QStringLiteral("loopback bind must succeed; got: %1").arg(err)));
    QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("err must be cleared on success; got: %1").arg(err)));
}

void TestWindowsBindHelper::canBindLocalAddress_rejects_null_address()
{
    QString err;
    QVERIFY(!WindowsBindHelper::canBindLocalAddress(QHostAddress(), &err));
    QVERIFY(!err.isEmpty());
}

void TestWindowsBindHelper::canBindLocalAddress_rejects_ipv6()
{
    // Only IPv4 source-binding is supported (matches the Linux side: the
    // VPN backends pin v4 too — IPv6 ngPost support is a separate concern).
    QString err;
    QVERIFY(!WindowsBindHelper::canBindLocalAddress(QHostAddress("::1"), &err));
    QVERIFY(err.contains(QStringLiteral("IPv4"), Qt::CaseInsensitive));
}

void TestWindowsBindHelper::canBindLocalAddress_rejects_unbound_address()
{
    // RFC5737 TEST-NET-1, guaranteed not to be on any interface here.
    QString err;
    QVERIFY(!WindowsBindHelper::canBindLocalAddress(QHostAddress("192.0.2.42"), &err));
    QVERIFY(!err.isEmpty());
}

void TestWindowsBindHelper::bindAndAttach_loopback_socket_reaches_bound_state()
{
    QTcpSocket sock;
    QString err;
    QVERIFY2(WindowsBindHelper::bindAndAttach(&sock, QHostAddress::LocalHost, &err),
             qPrintable(QStringLiteral("bindAndAttach on loopback must succeed; got: %1").arg(err)));
    QCOMPARE(sock.state(), QAbstractSocket::BoundState);
    QCOMPARE(sock.localAddress(), QHostAddress(QHostAddress::LocalHost));
    // ephemeral port assigned by the kernel — must be >0.
    QVERIFY(sock.localPort() > 0);
}

void TestWindowsBindHelper::bindAndAttach_rejects_null_socket()
{
    QString err;
    QVERIFY(!WindowsBindHelper::bindAndAttach(nullptr, QHostAddress::LocalHost, &err));
    QVERIFY(!err.isEmpty());
}

void TestWindowsBindHelper::bindAndAttach_rejects_null_address()
{
    QTcpSocket sock;
    QString err;
    QVERIFY(!WindowsBindHelper::bindAndAttach(&sock, QHostAddress(), &err));
    QVERIFY(!err.isEmpty());
}

#endif // Q_OS_WIN

QTEST_MAIN(TestWindowsBindHelper)
#include "tst_WindowsBindHelper.moc"
