// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_VpnSocketBinder.cpp — the contract that keeps NNTP traffic inside the
// tunnel.
//
// Every posting socket for a VPN-bound server is handed to
// VpnSocketBinder::bind() with the tunnel's local address. If that call ever
// reported success without the socket actually being bound to that address,
// the connection would still be made -- over the default route, outside the
// tunnel, with nothing in the log to say so. The failure is silent by nature,
// which is exactly why it needs assertions of its own rather than being left
// to a posting run to notice.
//
//========================================================================

#include <QtTest>

#include <QHostAddress>
#include <QNetworkInterface>
#include <QTcpSocket>

#include "vpn/VpnSocketBinder.h"

namespace
{
//! RFC 5737 TEST-NET-3, reserved for documentation and guaranteed never to be
//! a real local address. Standing in for "the tunnel address is gone".
QHostAddress unassignedAddress() { return QHostAddress(QStringLiteral("203.0.113.1")); }
} // namespace

class TestVpnSocketBinder : public QObject
{
    Q_OBJECT

private slots:
    //! A null socket is refused rather than dereferenced, and says why.
    void bind_refuses_a_null_socket();

    //! Binding to an address this machine does not hold must FAIL. Succeeding
    //! here is the leak: the socket would fall back to the default route and
    //! carry the post outside the tunnel.
    void bind_fails_on_an_address_the_machine_does_not_hold();

    //! A successful bind must leave the socket actually holding the address it
    //! was given -- a true return with an unbound socket would be the same
    //! leak wearing a success message.
    void bind_attaches_the_socket_to_the_requested_address();

    //! errMsg is cleared on entry, so a message left by an earlier failure
    //! cannot be mistaken for a diagnosis of the current call.
    void bind_clears_a_stale_error_message();

    //! errMsg is optional; passing nullptr must not crash on either path.
    void bind_tolerates_a_null_error_pointer();

    //! localAddressSummary() is what the log shows when a bind fails, so it
    //! has to actually name the addresses the machine holds.
    void local_address_summary_lists_the_real_addresses();
};

void TestVpnSocketBinder::bind_refuses_a_null_socket()
{
    QString err = QStringLiteral("untouched");
    QVERIFY(!VpnSocketBinder::bind(nullptr, QHostAddress::LocalHost, &err));
    QVERIFY2(!err.isEmpty(), "a refusal must say why");
    QVERIFY2(err != QStringLiteral("untouched"), "the caller's string was left as it was");
}

void TestVpnSocketBinder::bind_fails_on_an_address_the_machine_does_not_hold()
{
    QTcpSocket socket;
    QString err;
    const bool bound = VpnSocketBinder::bind(&socket, unassignedAddress(), &err);

    QVERIFY2(!bound, "binding to an address the machine does not hold reported success; "
                     "a posting socket would then leave through the default route");
    QVERIFY2(!err.isEmpty(), "a failed bind must report a reason for the log");
    QVERIFY2(socket.localAddress() != unassignedAddress(),
             "the socket claims an address the bind did not obtain");
}

void TestVpnSocketBinder::bind_attaches_the_socket_to_the_requested_address()
{
    QTcpSocket socket;
    QString err;
    QVERIFY2(VpnSocketBinder::bind(&socket, QHostAddress::LocalHost, &err),
             qPrintable(QStringLiteral("binding to loopback failed: %1").arg(err)));
    QVERIFY2(err.isEmpty(), "a successful bind must not leave an error message behind");

    // The point of the whole class: success means the socket really carries
    // that source address, not merely that the call returned true.
    QCOMPARE(socket.localAddress(), QHostAddress(QHostAddress::LocalHost));
    QVERIFY2(socket.localPort() != 0, "port 0 asks the kernel for an ephemeral port");
}

void TestVpnSocketBinder::bind_clears_a_stale_error_message()
{
    QTcpSocket socket;
    QString err = QStringLiteral("left over from an earlier attempt");
    QVERIFY(VpnSocketBinder::bind(&socket, QHostAddress::LocalHost, &err));
    QVERIFY2(err.isEmpty(),
             "a stale message survived a successful bind and would be reported as its diagnosis");
}

void TestVpnSocketBinder::bind_tolerates_a_null_error_pointer()
{
    QTcpSocket ok;
    QVERIFY(VpnSocketBinder::bind(&ok, QHostAddress::LocalHost, nullptr));

    QTcpSocket ko;
    QVERIFY(!VpnSocketBinder::bind(&ko, unassignedAddress(), nullptr));

    QVERIFY(!VpnSocketBinder::bind(nullptr, QHostAddress::LocalHost, nullptr));
}

void TestVpnSocketBinder::local_address_summary_lists_the_real_addresses()
{
    const QString summary = VpnSocketBinder::localAddressSummary();
    QVERIFY2(!summary.isEmpty(), "a machine running this test holds at least one address");

    // Whatever else the machine has, loopback is always there.
    bool const hasLoopback =
        summary.contains(QStringLiteral("127.0.0.1")) || summary.contains(QStringLiteral("::1"));
    QVERIFY2(hasLoopback, qPrintable(QStringLiteral("no loopback address in: %1").arg(summary)));

    for (QHostAddress const &address : QNetworkInterface::allAddresses())
        QVERIFY2(summary.contains(address.toString()),
                 qPrintable(QStringLiteral("'%1' is missing from: %2")
                                .arg(address.toString(), summary)));
}

QTEST_MAIN(TestVpnSocketBinder)
#include "tst_VpnSocketBinder.moc"
