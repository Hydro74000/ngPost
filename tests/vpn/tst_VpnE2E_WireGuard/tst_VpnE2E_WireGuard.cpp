//========================================================================
//
// tst_VpnE2E_WireGuard.cpp — full ngPost → mock NNTP traffic over a
// real WireGuard tunnel.
//
// Flow per test case:
//   1. Bring up a self-hosted WG server on 10.42.0.1 via the
//      tests/vpn/wg_server_up.sh helper (requires passwordless sudo +
//      wg-quick).
//   2. Stage a VpnProfile pointing at the generated client.conf inside
//      a sandboxed XDG_CONFIG_HOME, and an ngPost.conf that enables it.
//   3. Start the mock NNTP server bound to 0.0.0.0 so it accepts both
//      loopback and tunnel-side connections.
//   4. Run ngPost --vpn -h 10.42.0.1 -P <mock>. ngPost spawns the
//      privileged helper via $NGPOST_HELPER_LAUNCHER (sudo), which brings
//      up the client tunnel and signals READY.
//   5. After posting completes, parse the mock's log file to verify the
//      NNTP connection's peer IP is the tunnel client IP (10.42.0.2),
//      proving traffic actually exited via the tunnel — not loopback.
//   6. Tear down via wg_server_down.sh.
//
// Skipped automatically when any of (sudo, wg-quick, wireguard kernel
// module, ngPost binary) is unavailable, with a clear reason.
//
//========================================================================

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include "MockNntpServer.h"
#include "TestEnv.h"

using ngpost::tests::HomeSandbox;
using ngpost::tests::MockNntpServer;
using ngpost::tests::locateNgPostBinary;

namespace
{

QString runScriptsDir()
{
    return QString::fromLatin1(NGPOST_TESTS_ROOT) + QStringLiteral("/vpn");
}

//! True when the test environment can reasonably attempt a real WG E2E.
//! On a developer laptop without privileged container access this returns
//! false → the whole suite QSKIPs cleanly.
bool prerequisitesAvailable(QString *reason)
{
    if (QStandardPaths::findExecutable("wg-quick").isEmpty()
        || QStandardPaths::findExecutable("wg").isEmpty()) {
        *reason = QStringLiteral("wireguard-tools (wg, wg-quick) not on PATH");
        return false;
    }
    if (QStandardPaths::findExecutable("sudo").isEmpty()) {
        *reason = QStringLiteral("sudo not on PATH");
        return false;
    }
    // Probe passwordless sudo. `sudo -n true` returns 0 iff no password is
    // required (CI runners, dev boxes with NOPASSWD set).
    QProcess p;
    p.start("sudo", { "-n", "true" });
    p.waitForFinished(2000);
    if (p.exitCode() != 0) {
        *reason = QStringLiteral("sudo requires a password (set NOPASSWD or run as root)");
        return false;
    }
    return true;
}

//! Read the mock NNTP server's connection log into one string. Returns
//! empty if the log doesn't exist yet (test is still warming up).
QString readMockLog(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

//! Bring up the WG server. Returns the state dir on success or empty on
//! failure. Sets `clientIp` and `serverIp` on success.
QString wgServerUp(QString *serverIp, QString *clientIp, QString *errMsg)
{
    QTemporaryDir state;
    state.setAutoRemove(false);
    if (!state.isValid()) {
        *errMsg = QStringLiteral("failed to create WG state dir");
        return {};
    }

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("STATE_DIR", state.path());
    p.setProcessEnvironment(env);
    p.start("sudo", { "-n",
                      "-E",  // preserve STATE_DIR
                      "bash",
                      runScriptsDir() + QStringLiteral("/wg_server_up.sh") });
    if (!p.waitForFinished(15000)) {
        p.kill();
        *errMsg = QStringLiteral("wg_server_up.sh timed out: %1")
                      .arg(QString::fromLocal8Bit(p.readAll()));
        return {};
    }
    QString out = QString::fromLocal8Bit(p.readAll());
    if (p.exitCode() != 0) {
        *errMsg = QStringLiteral("wg_server_up.sh failed (code %1):\n%2")
                      .arg(p.exitCode()).arg(out);
        return {};
    }
    QFile sa(state.path() + "/server.addr"), ca(state.path() + "/client.addr");
    if (sa.open(QIODevice::ReadOnly)) *serverIp = QString::fromUtf8(sa.readAll()).trimmed();
    if (ca.open(QIODevice::ReadOnly)) *clientIp = QString::fromUtf8(ca.readAll()).trimmed();
    return state.path();
}

void wgServerDown(const QString &stateDir)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("STATE_DIR", stateDir);
    p.setProcessEnvironment(env);
    p.start("sudo", { "-n", "-E", "bash",
                      runScriptsDir() + QStringLiteral("/wg_server_down.sh") });
    p.waitForFinished(8000);
}

} // namespace

class TestVpnE2E_WireGuard : public QObject
{
    Q_OBJECT

private:
    QString _bin;
    QString _stateDir;
    QString _serverIp;
    QString _clientIp;

private slots:
    void initTestCase();
    void cleanupTestCase();

    //! ngPost posts a file with --vpn through a real WG tunnel; the mock
    //! NNTP log shows the connection's peer IP is the tunnel client IP,
    //! not loopback.
    void post_via_wireguard_tunnel_reaches_mock_via_tunnel_ip();
};

void TestVpnE2E_WireGuard::initTestCase()
{
    QString reason;
    if (!prerequisitesAvailable(&reason))
        QSKIP(qPrintable(QStringLiteral("WG E2E prerequisites missing: %1").arg(reason)));

    _bin = locateNgPostBinary();
    if (_bin.isEmpty())
        QSKIP("ngPost binary not found. Build it first or set NGPOST_BIN.");

    QString err;
    _stateDir = wgServerUp(&_serverIp, &_clientIp, &err);
    if (_stateDir.isEmpty())
        QSKIP(qPrintable(QStringLiteral("WG server bring-up failed: %1").arg(err)));

    qInfo() << "WG up: server=" << _serverIp << "client=" << _clientIp
            << "state=" << _stateDir;
}

void TestVpnE2E_WireGuard::cleanupTestCase()
{
    if (!_stateDir.isEmpty())
        wgServerDown(_stateDir);
}

void TestVpnE2E_WireGuard::post_via_wireguard_tunnel_reaches_mock_via_tunnel_ip()
{
    HomeSandbox sandbox;

    // 1) Stage the WG client config inside the sandboxed configDir/vpn/.
    const QString vpnDir = sandbox.xdgConfigHome() + "/ngPost/vpn";
    QVERIFY(QDir().mkpath(vpnDir));
    QVERIFY(QFile::copy(_stateDir + "/client.conf", vpnDir + "/test_wg.conf"));

    // 2) Stage an ngPost.conf with a [vpn_profile] block and the
    //    VPN_AUTO_CONNECT + VPN_ACTIVE_PROFILE keys flipped on.
    const QString confDir = sandbox.xdgConfigHome() + "/ngPost";
    QFile conf(confDir + "/ngPost.conf");
    QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
    {
        QTextStream ts(&conf);
        ts << "lang = EN\n";
        ts << "VPN_AUTO_CONNECT = false\n"; // CLI --vpn flips this to true
        ts << "VPN_ACTIVE_PROFILE = TestWG\n";
        ts << "\n[vpn_profile]\n";
        ts << "name        = TestWG\n";
        ts << "backend     = wireguard\n";
        ts << "config_file = test_wg.conf\n";
        ts << "has_auth    = false\n";
        ts << "\n[server]\n";
        ts << "host = " << _serverIp << "\n";
        ts << "port = 11119\n";  // placeholder, overridden below by -S
        ts << "ssl = false\n";
        ts << "user = u\n";
        ts << "pass = p\n";
        ts << "connection = 1\n";
        ts << "enabled = true\n";
        ts << "nzbCheck = false\n";
        ts << "useVpn = false\n"; // global --vpn handles routing
    }

    // 3) Start the mock NNTP. Bind to 0.0.0.0 so ngPost can reach it
    //    via the tunnel IP rather than loopback.
    MockNntpServer mock;
    {
        QStringList args = { "--listen", "0.0.0.0" };
        QVERIFY(mock.start(args));
    }
    qInfo() << "mock NNTP listening on 0.0.0.0:" << mock.port();

    // 4) Tiny payload to post.
    const QString inPath = sandbox.rootPath() + "/payload.bin";
    {
        QFile f(inPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("tunnel-traffic");
    }
    const QString nzbPath = sandbox.rootPath() + "/payload.nzb";

    // 5) Run ngPost with --vpn so VpnManager brings up the client tunnel
    //    via the helper. NGPOST_HELPER_LAUNCHER=sudo bypasses pkexec.
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HOME", sandbox.rootPath());
    env.insert("XDG_CONFIG_HOME", sandbox.xdgConfigHome());
    env.insert("NGPOST_HELPER_LAUNCHER", "sudo -n -E");
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(_bin, {
        "--vpn",
        "-S", QStringLiteral("u:p@@@%1:%2:1:nossl").arg(_serverIp).arg(mock.port()),
        "-i", inPath,
        "-o", nzbPath,
        "-g", "alt.binaries.test",
        "--disp_progress", "none",
    });
    QVERIFY(p.waitForStarted(5000));
    const bool finished = p.waitForFinished(60000);
    const QString output = QString::fromLocal8Bit(p.readAll());
    if (!finished) {
        p.kill();
        p.waitForFinished(2000);
        QFAIL(qPrintable(QStringLiteral("ngPost did not finish in time. Output:\n%1").arg(output)));
    }
    if (p.exitCode() != 0) {
        QFAIL(qPrintable(QStringLiteral("ngPost exit=%1, output:\n%2").arg(p.exitCode()).arg(output)));
    }

    // 6) NZB file produced.
    QVERIFY2(QFile::exists(nzbPath),
             qPrintable(QStringLiteral("NZB not written. ngPost output:\n%1").arg(output)));

    // 7) The mock saw the connection. Inspect the log: every accepted
    //    connection line starts with `[<ip>:<port>]`. We expect the
    //    `<ip>` to match the tunnel client IP, NOT 127.0.0.1.
    const QString log = readMockLog(mock.logFile());
    QVERIFY2(!log.isEmpty(), "mock log is empty — no connection observed");
    qInfo().noquote() << "mock log:\n" << log;

    QRegularExpression peerRe(QStringLiteral(R"(\[(\d+\.\d+\.\d+\.\d+):\d+\] connect)"));
    QRegularExpressionMatchIterator it = peerRe.globalMatch(log);
    bool sawTunnelIp = false;
    while (it.hasNext()) {
        const auto match = it.next();
        const QString peerIp = match.captured(1);
        if (peerIp == _clientIp) {
            sawTunnelIp = true;
            break;
        }
    }
    QVERIFY2(sawTunnelIp,
             qPrintable(QStringLiteral(
                 "Expected the NNTP connection's peer IP to be the tunnel "
                 "client (%1) — meaning traffic exited the VPN — but the "
                 "mock log only shows non-tunnel addresses. ngPost output:\n%2")
                            .arg(_clientIp).arg(output)));
}

QTEST_MAIN(TestVpnE2E_WireGuard)
#include "tst_VpnE2E_WireGuard.moc"
