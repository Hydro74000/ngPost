// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
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
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
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

QString helperScript()
{
    return QString::fromLatin1(NGPOST_SOURCE_ROOT)
           + QStringLiteral("/src/vpn/scripts/ngpost-vpn-helper.sh");
}

QString processStartTime(qint64 pid)
{
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("0");
    const QByteArray record = stat.readAll().trimmed();
    const int closeParen = record.lastIndexOf(')');
    if (closeParen < 0)
        return QStringLiteral("0");
    const QList<QByteArray> fields = record.mid(closeParen + 2).split(' ');
    return fields.size() > 19 ? QString::fromLatin1(fields.at(19))
                              : QStringLiteral("0");
}

bool waitForHelperMessage(QProcess *process, QByteArray const &keyword,
                          QByteArray *output, int timeoutMs = 40000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        output->append(process->readAll());
        for (QByteArray const &line : output->split('\n')) {
            if (line == keyword || line.startsWith(keyword + ' '))
                return true;
        }
        if (process->state() == QProcess::NotRunning)
            break;
        process->waitForReadyRead(qMin(500, timeoutMs - static_cast<int>(timer.elapsed())));
    }
    output->append(process->readAll());
    return false;
}

void startDirectHelper(QProcess *process, QString const &configPath,
                       qint64 ownerPid, QString const &ownerStart,
                       QString const &binDir = QString())
{
    process->setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = {
        QStringLiteral("-n"), QStringLiteral("-E"), QStringLiteral("bash"),
        helperScript(), QStringLiteral("wireguard"), configPath,
        QStringLiteral("--protocol"), QStringLiteral("2"),
        QStringLiteral("--owner-pid"), QString::number(ownerPid),
        QStringLiteral("--owner-start"), ownerStart,
        QStringLiteral("--wait-minutes"), QStringLiteral("0")
    };
    if (!binDir.isEmpty())
        args << QStringLiteral("--bin-dir") << binDir;
    process->start(QStringLiteral("sudo"), args);
}

bool writeWireGuardDump(QString const &path, qint64 firstHandshake,
                        quint64 firstTx, qint64 secondHandshake,
                        quint64 secondTx)
{
    QFile dump(path);
    if (!dump.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream stream(&dump);
    stream << "private\tpublic\t51820\toff\n"
           << "peer-a\t(none)\t127.0.0.1:1\t0.0.0.0/0\t"
           << firstHandshake << "\t0\t" << firstTx << "\t25\n"
           << "peer-b\t(none)\t127.0.0.1:2\t0.0.0.0/0\t"
           << secondHandshake << "\t0\t" << secondTx << "\t25\n";
    return stream.status() == QTextStream::Ok;
}

QByteArray cleanupAttributedState()
{
    QProcess cleanup;
    cleanup.setProcessChannelMode(QProcess::MergedChannels);
    cleanup.start(QStringLiteral("sudo"), {
        QStringLiteral("-n"), QStringLiteral("-E"), QStringLiteral("bash"),
        helperScript(), QStringLiteral("cleanup-stale"),
        QStringLiteral("--protocol"), QStringLiteral("2"),
        QStringLiteral("--owner-pid"), QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("--owner-start"), processStartTime(QCoreApplication::applicationPid()),
        QStringLiteral("--nonblocking")
    });
    cleanup.waitForFinished(15000);
    return cleanup.readAll();
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

void wgServerDown(const QString &stateDir);

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
    p.start("sudo", { "-n",
                      QStringLiteral("STATE_DIR=%1").arg(state.path()),
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
    if (serverIp->isEmpty() || clientIp->isEmpty()
        || !QFileInfo::exists(state.path() + "/client.conf")) {
        *errMsg = QStringLiteral("wg_server_up.sh completed but did not populate %1. Output:\n%2")
                      .arg(state.path(), out);
        wgServerDown(state.path());
        return {};
    }
    return state.path();
}

void wgServerDown(const QString &stateDir)
{
    QProcess p;
    p.start("sudo", { "-n", QStringLiteral("STATE_DIR=%1").arg(stateDir), "bash",
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
    void parent_eof_alone_stops_and_cleans_helper();
    void parent_watchdog_alone_stops_and_cleans_helper();
    void helper_sigkill_releases_lease_and_next_run_cleans_manifest();
    void wireguard_health_aggregates_all_emitting_peers();
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
    cleanupAttributedState();
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

void TestVpnE2E_WireGuard::parent_eof_alone_stops_and_cleans_helper()
{
    // The watched owner remains alive throughout: only stdin EOF can stop
    // this helper, so the watchdog cannot hide a broken EOF path.
    QProcess helper;
    QByteArray output;
    const qint64 owner = QCoreApplication::applicationPid();
    startDirectHelper(&helper, _stateDir + QStringLiteral("/client.conf"),
                      owner, processStartTime(owner));
    QVERIFY2(helper.waitForStarted(5000), qPrintable(helper.errorString()));
    QVERIFY2(waitForHelperMessage(&helper, QByteArrayLiteral("READY"), &output),
             output.constData());

    helper.closeWriteChannel();
    QVERIFY2(helper.waitForFinished(15000), "helper ignored stdin EOF");
    output += helper.readAll();
    QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
    QCOMPARE(helper.exitCode(), 0);
    QVERIFY2(!QFileInfo::exists(QStringLiteral("/sys/class/net/ngpost-wg0")),
             output.constData());
    QVERIFY(!QFileInfo::exists(QStringLiteral("/run/ngpost-vpn/owner-v2")));
}

void TestVpnE2E_WireGuard::parent_watchdog_alone_stops_and_cleans_helper()
{
    // Keep helper stdin open and kill only the watched owner. This proves the
    // PID/start-time watchdog independently of the EOF protection.
    QProcess owner;
    owner.start(QStringLiteral("sleep"), {QStringLiteral("60")});
    QVERIFY(owner.waitForStarted(5000));
    const qint64 ownerPid = owner.processId();

    QProcess helper;
    QByteArray output;
    startDirectHelper(&helper, _stateDir + QStringLiteral("/client.conf"),
                      ownerPid, processStartTime(ownerPid));
    QVERIFY2(helper.waitForStarted(5000), qPrintable(helper.errorString()));
    QVERIFY2(waitForHelperMessage(&helper, QByteArrayLiteral("READY"), &output),
             output.constData());

    owner.kill();
    owner.waitForFinished(5000);
    QVERIFY2(helper.waitForFinished(15000), "helper watchdog did not notice parent death");
    output += helper.readAll();
    QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
    QVERIFY2(!QFileInfo::exists(QStringLiteral("/sys/class/net/ngpost-wg0")),
             output.constData());
    QVERIFY(!QFileInfo::exists(QStringLiteral("/run/ngpost-vpn/owner-v2")));
}

void TestVpnE2E_WireGuard::helper_sigkill_releases_lease_and_next_run_cleans_manifest()
{
    QProcess helper;
    QByteArray output;
    const qint64 owner = QCoreApplication::applicationPid();
    startDirectHelper(&helper, _stateDir + QStringLiteral("/client.conf"),
                      owner, processStartTime(owner));
    QVERIFY2(helper.waitForStarted(5000), qPrintable(helper.errorString()));
    QVERIFY2(waitForHelperMessage(&helper, QByteArrayLiteral("READY"), &output),
             output.constData());

    QFile lease(QStringLiteral("/run/lock/ngpost-vpn.lock"));
    QVERIFY(lease.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray ownerRecord = lease.readAll();
    const QRegularExpression helperPidPattern(QStringLiteral(R"((?:^| )helper_pid=(\d+)(?: |$))"));
    const QRegularExpressionMatch match = helperPidPattern.match(QString::fromLatin1(ownerRecord));
    QVERIFY2(match.hasMatch(), ownerRecord.constData());
    const QString helperPid = match.captured(1);

    QCOMPARE(QProcess::execute(QStringLiteral("sudo"),
                               {QStringLiteral("-n"), QStringLiteral("kill"),
                                QStringLiteral("-KILL"), helperPid}), 0);
    QVERIFY(helper.waitForFinished(10000));
    QVERIFY(QFileInfo::exists(QStringLiteral("/run/ngpost-vpn/owner-v2")));
    QVERIFY(QFileInfo::exists(QStringLiteral("/sys/class/net/ngpost-wg0")));

    // flock is the authority and must have vanished with the helper even
    // though its manifest and network objects intentionally remain.
    QCOMPARE(QProcess::execute(QStringLiteral("sudo"),
                               {QStringLiteral("-n"), QStringLiteral("flock"),
                                QStringLiteral("-n"),
                                QStringLiteral("/run/lock/ngpost-vpn.lock"),
                                QStringLiteral("true")}), 0);

    const QByteArray cleanupOutput = cleanupAttributedState();
    QVERIFY2(cleanupOutput.contains("CLEANED"), cleanupOutput.constData());
    QVERIFY(!QFileInfo::exists(QStringLiteral("/sys/class/net/ngpost-wg0")));
    QVERIFY(!QFileInfo::exists(QStringLiteral("/run/ngpost-vpn/owner-v2")));
}

void TestVpnE2E_WireGuard::wireguard_health_aggregates_all_emitting_peers()
{
    // Keep the real network-object setup, but replace only `wg setconf/show`
    // with a deterministic dump. This exercises the helper's production
    // per-peer state machine and global aggregation, including the 25-second
    // confirmation window, without depending on two external peers.
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString dumpPath = fixture.path() + QStringLiteral("/wg.dump");
    const QString fakeWgPath = fixture.path() + QStringLiteral("/wg");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QVERIFY(writeWireGuardDump(dumpPath, now, 10, now, 10));

    QFile fakeWg(fakeWgPath);
    QVERIFY(fakeWg.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    QTextStream script(&fakeWg);
    script << "#!/usr/bin/env bash\n"
              "set -eu\n"
              "if [ \"${1:-}\" = setconf ]; then exit 0; fi\n"
              "if [ \"${1:-}\" = show ] && [ \"${3:-}\" = dump ]; then\n"
              "  exec /bin/cat " << dumpPath << "\n"
              "fi\n"
              "exit 1\n";
    QVERIFY(script.status() == QTextStream::Ok);
    fakeWg.close();
    QVERIFY(fakeWg.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                  | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                  | QFileDevice::ExeOther));

    QProcess helper;
    QByteArray output;
    const qint64 owner = QCoreApplication::applicationPid();
    startDirectHelper(&helper, _stateDir + QStringLiteral("/client.conf"),
                      owner, processStartTime(owner), fixture.path());
    QVERIFY2(helper.waitForStarted(5000), qPrintable(helper.errorString()));
    QVERIFY2(waitForHelperMessage(&helper, QByteArrayLiteral("READY"), &output),
             output.constData());
    QCOMPARE(helper.write("ACTIVE\n"), qint64(7));
    QVERIFY(helper.waitForBytesWritten(2000));

    // Let the helper establish its baseline, then make both peers emit. One
    // handshake is stale and one is fresh: the aggregate may become SUSPECT,
    // but must not emit DOWN while an emitting peer remains healthy.
    QTest::qWait(1500);
    QVERIFY(writeWireGuardDump(dumpPath, now - 200, 11, now, 11));
    QVERIFY2(waitForHelperMessage(&helper, QByteArrayLiteral("SUSPECT"), &output, 6000),
             output.constData());
    QVERIFY2(!output.contains("\nDOWN ") && !output.startsWith("DOWN "),
             output.constData());

    // Once every emitting peer is stale and the confirmation period has
    // elapsed, one global DOWN is expected.
    QTest::qWait(25000);
    QVERIFY(writeWireGuardDump(dumpPath, now - 230, 11, now - 230, 11));
    QVERIFY2(waitForHelperMessage(&helper, QByteArrayLiteral("DOWN"), &output, 6000),
             output.constData());
    QCOMPARE(output.count("DOWN backend=wireguard"), 1);

    helper.closeWriteChannel();
    QVERIFY2(helper.waitForFinished(15000), "helper ignored stdin EOF after health test");
    output += helper.readAll();
    QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
    QCOMPARE(helper.exitCode(), 0);
    QVERIFY2(!QFileInfo::exists(QStringLiteral("/sys/class/net/ngpost-wg0")),
             output.constData());
}

QTEST_MAIN(TestVpnE2E_WireGuard)
#include "tst_VpnE2E_WireGuard.moc"
