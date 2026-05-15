//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "WireGuardBackend.h"

#include "VpnManager.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTimer>

WireGuardBackend::WireGuardBackend(QObject *parent)
    : VpnBackend(parent)
    , _proc(nullptr)
    , _stdoutBuffer()
    , _readySignaled(false)
#ifdef Q_OS_WIN
    , _winServiceName()
    , _winIface()
    , _winPollAttempts(0)
    , _winPollTimer(nullptr)
#endif
{}

WireGuardBackend::~WireGuardBackend()
{
    if (_proc) {
        if (_proc->state() != QProcess::NotRunning) {
            _proc->closeWriteChannel();
            if (!_proc->waitForFinished(3000))
                _proc->kill();
        }
        delete _proc;
        _proc = nullptr;
    }
}

bool WireGuardBackend::start(QString const &configPathPacked)
{
#ifdef Q_OS_WIN
    // Drive the registered WireGuard tunnel service via the Service Control
    // Manager. Keep locals scoped away from the Linux path below.
    {
        QString winCfg = configPathPacked;
        int nul = configPathPacked.indexOf(QChar(QChar::Null));
        if (nul >= 0)
            winCfg = configPathPacked.left(nul);
        return _startWindows(winCfg);
    }
#elif !defined(Q_OS_LINUX)
    Q_UNUSED(configPathPacked);
    emit failed(tr("Native VPN integration is currently Linux-only. "
                   "macOS support is in progress."));
    return false;
#endif

    if (_proc && _proc->state() != QProcess::NotRunning)
        return true;

    // The VpnManager may encode auth-file path after \0 for OpenVPN; WireGuard
    // doesn't use auth files (uses keys in the .conf) but we accept the format
    // and ignore the trailer.
    QString configPath = configPathPacked;
    int nulIdx = configPathPacked.indexOf(QChar(QChar::Null));
    if (nulIdx >= 0)
        configPath = configPathPacked.left(nulIdx);

    QFileInfo fi(configPath);
    if (!fi.exists() || !fi.isReadable()) {
        emit failed(tr("Config file not found or unreadable: %1").arg(configPath));
        return false;
    }

    QString helper = VpnManager::helperScriptPath();
    if (helper.isEmpty()) {
        emit failed(tr("Privileged helper script ngpost-vpn-helper.sh not found"));
        return false;
    }

    _readySignaled = false;
    _stdoutBuffer.clear();

    _proc = new QProcess(this);
    _proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(_proc, &QProcess::readyReadStandardOutput,
            this, &WireGuardBackend::onReadyReadStdout);
    connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &WireGuardBackend::onProcessFinished);
    connect(_proc, &QProcess::errorOccurred,
            this, &WireGuardBackend::onProcessError);

    QStringList args;
    args << helper << "wireguard" << fi.absoluteFilePath();

    emit logLine(tr("Launching VPN helper: pkexec %1").arg(args.join(' ')));
    _proc->start(QStringLiteral("pkexec"), args);
    if (!_proc->waitForStarted(5000)) {
        emit failed(tr("Failed to start pkexec/helper"));
        delete _proc;
        _proc = nullptr;
        return false;
    }
    return true;
}

void WireGuardBackend::stop()
{
#ifdef Q_OS_WIN
    _stopWindows();
    return;
#endif
    if (!_proc) {
        emit stopped();
        return;
    }
    if (_proc->state() == QProcess::NotRunning) {
        emit stopped();
        return;
    }
    _proc->closeWriteChannel();
}

void WireGuardBackend::stopAndWait(int timeoutMs)
{
    stop();
#ifdef Q_OS_WIN
    Q_UNUSED(timeoutMs);
    return;
#endif
    if (_proc && _proc->state() != QProcess::NotRunning)
        _proc->waitForFinished(timeoutMs);
}

bool WireGuardBackend::isRunning() const
{
#ifdef Q_OS_WIN
    return !_winServiceName.isEmpty();
#endif
    return _proc && _proc->state() != QProcess::NotRunning;
}

void WireGuardBackend::onReadyReadStdout()
{
    if (!_proc)
        return;
    _stdoutBuffer.append(_proc->readAllStandardOutput());
    int nl;
    while ((nl = _stdoutBuffer.indexOf('\n')) >= 0) {
        QByteArray rawLine = _stdoutBuffer.left(nl);
        _stdoutBuffer.remove(0, nl + 1);
        _handleLine(QString::fromLocal8Bit(rawLine).trimmed());
    }
}

void WireGuardBackend::_handleLine(QString const &line)
{
    if (line.isEmpty())
        return;
    if (line.startsWith(QLatin1String("READY "))) {
        QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            QHostAddress ip(parts.at(2));
            if (!ip.isNull()) {
                QHostAddress dns;
                if (parts.size() >= 4 && parts.at(3) != QLatin1String("-"))
                    dns = QHostAddress(parts.at(3));
                _readySignaled = true;
                emit ready(parts.at(1), ip, dns);
                return;
            }
        }
        emit logLine(tr("Malformed READY from helper: %1").arg(line));
    }
    else if (line.startsWith(QLatin1String("ERROR "))) {
        emit failed(line.mid(6));
    }
    else if (line.startsWith(QLatin1String("LOG "))) {
        emit logLine(line.mid(4));
    }
    else {
        emit logLine(line);
    }
}

void WireGuardBackend::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    if (!_readySignaled)
        emit failed(tr("helper exited (code %1) before tunnel was ready").arg(exitCode));
    emit stopped();
}

void WireGuardBackend::onProcessError(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart)
        emit failed(tr("pkexec/helper failed to start"));
    else if (err == QProcess::Crashed)
        emit failed(tr("helper process crashed"));
}

#ifdef Q_OS_WIN
// ===========================================================================
//  Windows backend — drive the per-tunnel WireGuard service via SCM.
// ===========================================================================
//
// At profile-import time on Windows, ngPost runs (with UAC elevation):
//   wireguard.exe /installtunnelservice <copy-of-.conf-under-configDir>
// This creates a service named "WireGuardTunnel$<basename>" that runs
// in SYSTEM context and brings up the tunnel via Wintun. The service is
// configured as start=demand. ngPost then applies an ACL via `sc sdset`
// granting START / STOP to the current user, so that subsequent runtime
// Connect / Disconnect issued from the unprivileged ngPost process do
// not trigger UAC.
//
// Runtime here (no UAC):
//   - Connect    : sc start WireGuardTunnel$<name>     → wait RUNNING
//   - Disconnect : sc stop  WireGuardTunnel$<name>
//   - Query info : wg.exe show <name>  → iface, ip, dns
//
// We use sc.exe via QProcess rather than calling the Win32 SCM API directly
// because it keeps the code paths uniform with the rest of the project (we
// already shell out to a binary on Linux too) and avoids pulling in
// advapi32-only types into headers that should compile under both OSes.

#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

QString WireGuardBackend::_serviceNameFromConfig(QString const &configPath) const
{
    // The service name is "WireGuardTunnel$" + the basename of the config
    // file *without* its .conf extension. wireguard.exe uses this convention
    // when /installtunnelservice is called.
    QFileInfo fi(configPath);
    return QStringLiteral("WireGuardTunnel$") + fi.completeBaseName();
}

bool WireGuardBackend::_queryTunnelInfo(QString *iface, QString *ip, QString *dns) const
{
    // After the service brings the tunnel up, query its state with wg.exe.
    // Output format (per WireGuard docs):
    //   interface: ngpost-foo
    //   public key: ...
    //   listening port: 51820
    QString wgPath = QStandardPaths::findExecutable(QStringLiteral("wg.exe"));
    if (wgPath.isEmpty())
        wgPath = QStringLiteral("C:/Program Files/WireGuard/wg.exe");

    QFileInfo fi(_winServiceName);
    // wg show takes the iface name = service name without the prefix.
    QString tunnelName = _winServiceName;
    tunnelName.remove(QStringLiteral("WireGuardTunnel$"));

    QProcess p;
    p.start(wgPath, {QStringLiteral("show"), tunnelName});
    if (!p.waitForFinished(3000))
        return false;
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    if (p.exitCode() != 0 || out.isEmpty())
        return false;

    // wg show doesn't surface the local IP nor DNS directly — those live in
    // the [Interface] section of the .conf. Parse the .conf for them
    // instead; the service must have applied them as-is.
    QString cfgPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                      + "/vpn/" + tunnelName + ".conf";
    QFile cfg(cfgPath);
    if (!cfg.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QString content = QString::fromUtf8(cfg.readAll());
    cfg.close();

    QRegularExpression reAddr(
        QStringLiteral("(?im)^\\s*Address\\s*=\\s*(\\d+\\.\\d+\\.\\d+\\.\\d+)"));
    QRegularExpressionMatch m = reAddr.match(content);
    if (m.hasMatch())
        *ip = m.captured(1);

    QRegularExpression reDns(
        QStringLiteral("(?im)^\\s*DNS\\s*=\\s*([0-9.,\\s]+)"));
    m = reDns.match(content);
    if (m.hasMatch())
        *dns = m.captured(1).split(',').first().trimmed();

    *iface = tunnelName;
    return !ip->isEmpty();
}

bool WireGuardBackend::_startWindows(QString const &configPath)
{
    QFileInfo fi(configPath);
    if (!fi.exists()) {
        emit failed(tr("WireGuard config not found: %1").arg(configPath));
        return false;
    }
    _winServiceName = _serviceNameFromConfig(configPath);
    _winPollAttempts = 0;

    emit logLine(tr("Starting WireGuard service: %1").arg(_winServiceName));

    QProcess sc;
    sc.start(QStringLiteral("sc.exe"),
             {QStringLiteral("start"), _winServiceName});
    if (!sc.waitForFinished(5000)) {
        emit failed(tr("sc start timed out — is the tunnel registered? "
                       "(re-import the profile)"));
        return false;
    }
    int code = sc.exitCode();
    // sc.exe exits 1056 = "service is already running", which we accept.
    if (code != 0 && code != 1056) {
        emit failed(tr("sc start failed (exit %1) — make sure the WireGuard "
                       "tunnel was installed via wireguard.exe /installtunnelservice "
                       "and that runtime ACL grants you START.").arg(code));
        return false;
    }

    // Poll briefly for the tunnel to actually come up (wg show returns OK).
    if (!_winPollTimer) {
        _winPollTimer = new QTimer(this);
        _winPollTimer->setSingleShot(false);
        connect(_winPollTimer, &QTimer::timeout, this, &WireGuardBackend::onWinPollTimer);
    }
    _winPollTimer->start(500);
    return true;
}

void WireGuardBackend::onWinPollTimer()
{
    if (++_winPollAttempts > 40) { // 40 * 500ms = 20s
        _winPollTimer->stop();
        emit failed(tr("WireGuard tunnel did not come up within 20s"));
        return;
    }
    QString iface, ip, dns;
    if (!_queryTunnelInfo(&iface, &ip, &dns))
        return;
    _winPollTimer->stop();
    QHostAddress ipAddr(ip);
    QHostAddress dnsAddr = dns.isEmpty() ? QHostAddress() : QHostAddress(dns);
    if (ipAddr.isNull()) {
        emit failed(tr("Could not parse local IP from WireGuard config"));
        return;
    }
    _winIface = iface;
    emit ready(iface, ipAddr, dnsAddr);
}

void WireGuardBackend::_stopWindows()
{
    if (_winPollTimer)
        _winPollTimer->stop();
    if (_winServiceName.isEmpty()) {
        emit stopped();
        return;
    }
    QProcess sc;
    sc.start(QStringLiteral("sc.exe"),
             {QStringLiteral("stop"), _winServiceName});
    sc.waitForFinished(5000);
    // We don't strictly need to verify exit code: a failed stop just
    // leaves the tunnel up; the next session will detect.
    _winServiceName.clear();
    emit stopped();
}
#endif // Q_OS_WIN
